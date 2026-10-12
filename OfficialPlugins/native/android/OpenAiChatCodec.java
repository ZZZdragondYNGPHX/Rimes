package org.scholay.rimes.android;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

/** In-memory Chat Completions JSON and SSE codec. It has no endpoint, credentials or transport. */
final class OpenAiChatCodec {
    static final int MAX_TEXT_UNITS=16384,MAX_LINE_BYTES=131072,MAX_WIRE_BYTES=1048576;
    static final String MOCK_MODEL="rimes-local-mock";
    enum Code { EMPTY_INPUT,INPUT_LIMIT,OUTPUT_LIMIT,INVALID_UNICODE,INVALID_UTF8,INVALID_REQUEST,
        INVALID_FRAME,INCOMPLETE,PROVIDER_ERROR,WIRE_LIMIT,INVALID_DIRECTION,DICTIONARY_UNCOVERED,DICTIONARY_UNAVAILABLE,
        NOT_CONFIGURED,NETWORK_ERROR,HTTP_ERROR }
    static final class Failure extends Exception {
        final Code code;
        Failure(Code code,String message) { super(message); this.code=code; }
    }
    interface Listener { void onText(String text,boolean complete); }
    static final class Request {
        final String pluginID,model,instruction,source;
        Request(String pluginID,String model,String instruction,String source) {
            this.pluginID=pluginID; this.model=model; this.instruction=instruction; this.source=source;
        }
    }

    private OpenAiChatCodec() {}
    static String instruction(String pluginID) throws Failure {
        if("ask".equals(pluginID)) return "用提问的语言口语回答，2 到 4 句，先给结论，再补关键原因或例子。";
        // Mirrors Shared/Sources/RimesCore/AI.swift AIPrompt.polish.
        if("polish".equals(pluginID)) return "Polish the supplied text without changing its meaning or language. Return only the polished text.";
        if("poem".equals(pluginID)) return "根据提供的主题写四行中文短诗；只返回诗。";
        if("art".equals(pluginID)) return "把提供的描述转换成纯文本字符画提示词；不输出图片，不声称生成图片。";
        throw new Failure(Code.INVALID_REQUEST,"未知 AI 插件，原文已保留。");
    }
    static void validateSource(String source) throws Failure {
        if(source==null || source.trim().isEmpty()) throw new Failure(Code.EMPTY_INPUT,"先在输入行写下内容。");
        if(source.length()>MAX_TEXT_UNITS) throw new Failure(Code.INPUT_LIMIT,"原文超过 16384 个 UTF-16 单元，原文已保留。");
        validateUnicode(source);
    }
    static void validateUnicode(String value) throws Failure {
        for(int i=0;i<value.length();i++) {
            char c=value.charAt(i);
            if(Character.isHighSurrogate(c)) {
                if(i+1==value.length() || !Character.isLowSurrogate(value.charAt(++i)))
                    throw new Failure(Code.INVALID_UNICODE,"文本包含不完整的 Unicode 字符，原文已保留。");
            } else if(Character.isLowSurrogate(c))
                throw new Failure(Code.INVALID_UNICODE,"文本包含不完整的 Unicode 字符，原文已保留。");
        }
    }
    static byte[] makeRequest(String pluginID,String source) throws Failure {
        validateSource(source);
        try {
            JSONArray messages=new JSONArray().put(new JSONObject().put("role","system").put("content",instruction(pluginID)))
                    .put(new JSONObject().put("role","user").put("content",source));
            return new JSONObject().put("model",MOCK_MODEL).put("messages",messages).put("stream",true)
                    .toString().getBytes(StandardCharsets.UTF_8);
        } catch(JSONException error) { throw new Failure(Code.INVALID_REQUEST,"AI 请求格式无效，原文已保留。"); }
    }
    static byte[] makeRemoteRequest(String pluginID,String source,String model,String direction) throws Failure {
        return makeRemoteRequest(pluginID,source,model,direction,null);
    }
    static byte[] makeRemoteRequest(String pluginID,String source,String model,String direction,String packageInstruction) throws Failure {
        validateSource(source);
        if(packageInstruction!=null && (packageInstruction.trim().isEmpty() || packageInstruction.getBytes(StandardCharsets.UTF_8).length>32768)) throw invalidRequest();
        if(packageInstruction!=null) validateUnicode(packageInstruction);
        if(!OpenAiSettings.validModel(model)) throw invalidRequest();
        String prompt;
        if("translate".equals(pluginID)) {
            if(!"auto".equals(direction) && !"zh-en".equals(direction) && !"en-zh".equals(direction))
                throw new Failure(Code.INVALID_DIRECTION,"请选择自动、中译英或英译中，原文已保留。");
            boolean english="zh-en".equals(direction) || "auto".equals(direction)
                    && source.codePoints().anyMatch(c -> Character.UnicodeScript.of(c)==Character.UnicodeScript.HAN);
            prompt="Translate the supplied text into "+(english?"English":"Simplified Chinese")
                    +". Preserve meaning and tone. Return only the translation, without commentary.";
            if(packageInstruction!=null) prompt=packageInstruction.replace("{targetLanguage}",english?"English":"Simplified Chinese");
        } else prompt=packageInstruction==null?instruction(pluginID):packageInstruction;
        try {
            JSONObject request=new JSONObject().put("model",model).put("stream",true).put("max_tokens",2048)
                    .put("messages",new JSONArray().put(new JSONObject().put("role","system").put("content",prompt))
                            .put(new JSONObject().put("role","user").put("content",source)));
            if(model.startsWith("deepseek")) request.put("thinking",new JSONObject().put("type","disabled"));
            return request.toString().getBytes(StandardCharsets.UTF_8);
        } catch(JSONException error) { throw invalidRequest(); }
    }
    static Request readRequest(byte[] bytes) throws Failure {
        if(bytes==null || bytes.length>MAX_LINE_BYTES) throw new Failure(Code.INVALID_REQUEST,"AI 请求格式或长度无效，原文已保留。");
        try {
            JSONObject root=parseObject(decodeUtf8(bytes));
            if(!MOCK_MODEL.equals(root.opt("model")) || !Boolean.TRUE.equals(root.opt("stream"))) throw invalidRequest();
            JSONArray messages=root.getJSONArray("messages");
            if(messages.length()!=2) throw invalidRequest();
            JSONObject system=messages.getJSONObject(0),user=messages.getJSONObject(1);
            if(!"system".equals(system.opt("role")) || !"user".equals(user.opt("role"))
                    || !(system.opt("content") instanceof String) || !(user.opt("content") instanceof String)) throw invalidRequest();
            String prompt=(String)system.opt("content"),source=(String)user.opt("content"),plugin=null;
            for(String id:new String[]{"ask","polish","poem","art"}) if(instruction(id).equals(prompt)) { plugin=id; break; }
            if(plugin==null) throw invalidRequest();
            validateSource(source); validateUnicode(prompt);
            return new Request(plugin,MOCK_MODEL,prompt,source);
        } catch(JSONException error) { throw invalidRequest(); }
    }
    private static Failure invalidRequest() { return new Failure(Code.INVALID_REQUEST,"AI 请求格式无效，原文已保留。"); }
    private static JSONObject parseObject(String source) throws JSONException {
        JSONTokener input=new JSONTokener(source);
        Object value=input.nextValue();
        if(!(value instanceof JSONObject)) throw new JSONException("Expected one complete JSON object");
        // nextClean() also consumes JavaScript comments and treats a literal NUL as EOF.
        // Only JSON whitespace may follow the object; inspect the raw remaining characters.
        while(input.more()) {
            char extra=input.next();
            if(extra!=' ' && extra!='\t' && extra!='\r' && extra!='\n')
                throw new JSONException("Unexpected data after JSON object");
        }
        return (JSONObject)value;
    }
    private static String decodeUtf8(byte[] bytes) throws Failure {
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        } catch(CharacterCodingException error) { throw new Failure(Code.INVALID_UTF8,"流式回复不是有效 UTF-8，原文已保留。"); }
    }

    /** Handles fragmented UTF-8, CRLF, SSE comments and multiline data without guessing completion. */
    static final class Decoder {
        private final PluginCancellation cancellation;
        private final Listener listener;
        private final ByteArrayOutputStream line=new ByteArrayOutputStream();
        private final StringBuilder event=new StringBuilder(),text=new StringBuilder();
        private int wireBytes;
        private boolean stopped,done,completed;
        Decoder(PluginCancellation cancellation,Listener listener) {
            if(cancellation==null || listener==null) throw new IllegalArgumentException("Decoder dependencies are required");
            this.cancellation=cancellation; this.listener=listener;
        }
        void append(byte[] bytes) throws Failure {
            if(bytes==null) throw new IllegalArgumentException("Bytes are required");
            append(bytes,0,bytes.length);
        }
        void append(byte[] bytes,int offset,int length) throws Failure {
            if(bytes==null || offset<0 || length<0 || offset>bytes.length-length) throw new IllegalArgumentException("Invalid byte range");
            cancellation.check();
            if(length>MAX_WIRE_BYTES-wireBytes) throw new Failure(Code.WIRE_LIMIT,"流式回复过大，原文已保留。");
            wireBytes+=length;
            for(int i=offset;i<offset+length;i++) {
                if((i&255)==0) cancellation.check();
                if(bytes[i]=='\n') consumeLine();
                else {
                    if(line.size()>=MAX_LINE_BYTES) throw new Failure(Code.WIRE_LIMIT,"流式回复单行过大，原文已保留。");
                    line.write(bytes[i]);
                }
            }
        }
        String text() { return text.toString(); }
        String finish() throws Failure {
            cancellation.check();
            if(!done || line.size()!=0 || event.length()!=0 || text.length()==0)
                throw new Failure(Code.INCOMPLETE,"流式回复未完整结束，原文已保留。");
            if(!completed) { completed=true; listener.onText(text.toString(),true); }
            return text.toString();
        }
        private void consumeLine() throws Failure {
            String value=decodeUtf8(line.toByteArray()); line.reset();
            if(value.endsWith("\r")) value=value.substring(0,value.length()-1);
            if(value.isEmpty()) { consumeEvent(); return; }
            if(value.startsWith("data:")) {
                String data=value.substring(5); if(data.startsWith(" ")) data=data.substring(1);
                if(event.length()+data.length()+1>MAX_LINE_BYTES) throw new Failure(Code.WIRE_LIMIT,"流式回复事件过大，原文已保留。");
                if(event.length()>0) event.append('\n'); event.append(data);
            }
        }
        private void consumeEvent() throws Failure {
            if(event.length()==0) return;
            String payload=event.toString(); event.setLength(0); cancellation.check();
            if(done) throw invalidFrame();
            if("[DONE]".equals(payload)) {
                if(!stopped || text.length()==0) throw new Failure(Code.INCOMPLETE,"流式回复缺少成功结束标记，原文已保留。");
                done=true; return;
            }
            try {
                JSONObject root=parseObject(payload);
                if(root.has("error")) throw new Failure(Code.PROVIDER_ERROR,"AI 服务返回错误，原文已保留。");
                if(!"chat.completion.chunk".equals(root.opt("object"))) throw invalidFrame();
                JSONArray choices=root.getJSONArray("choices");
                if(choices.length()==0) return; // Optional usage-only chunk.
                if(choices.length()!=1) throw invalidFrame();
                JSONObject choice=choices.getJSONObject(0);
                if(!(choice.opt("index") instanceof Number) || ((Number)choice.opt("index")).doubleValue()!=0) throw invalidFrame();
                JSONObject delta=choice.getJSONObject("delta");
                Object content=delta.opt("content"),reason=choice.opt("finish_reason");
                if(content!=null && content!=JSONObject.NULL && !(content instanceof String)) throw invalidFrame();
                if(reason!=null && reason!=JSONObject.NULL && !(reason instanceof String)) throw invalidFrame();
                if(content instanceof String && !((String)content).isEmpty()) {
                    if(stopped) throw invalidFrame();
                    String part=(String)content; validateUnicode(part);
                    if(part.length()>MAX_TEXT_UNITS-text.length()) throw new Failure(Code.OUTPUT_LIMIT,"输出超过 16384 个 UTF-16 单元，原文已保留。");
                    text.append(part); cancellation.check(); listener.onText(text.toString(),false);
                }
                if(reason instanceof String) {
                    if(stopped || !"stop".equals(reason)) throw new Failure(Code.INCOMPLETE,"流式回复未成功完成，原文已保留。");
                    stopped=true;
                }
            } catch(JSONException error) { throw invalidFrame(); }
        }
        private Failure invalidFrame() { return new Failure(Code.INVALID_FRAME,"流式回复格式无效，原文已保留。"); }
    }
}
