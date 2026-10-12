package org.scholay.rimes.android;

import android.content.Context;
import java.io.IOException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** One plugin worker. Callbacks run here; the service owns main-thread target authority. */
final class BufferPluginExecutor implements AutoCloseable {
    interface Listener {
        void onUpdate(String text,boolean complete);
        void onFailure(String message);
    }
    private final ThreadPoolExecutor worker=new ThreadPoolExecutor(1,1,0,TimeUnit.MILLISECONDS,
            new LinkedBlockingQueue<>(),task -> { Thread thread=new Thread(task,"RIMES-plugins"); thread.setDaemon(true); return thread; });
    private final OfflineDictionary dictionary;
    private final OpenAiSettings aiSettings;
    private final OfficialPluginStore packages;
    private Job current;
    private boolean closed;

    BufferPluginExecutor(Context context) {
        if(context==null) throw new IllegalArgumentException("Plugin context is required");
        dictionary=new OfflineDictionary(context.getApplicationContext());
        aiSettings=new OpenAiSettings(context.getApplicationContext());
        packages=new OfficialPluginStore(context);
    }
    synchronized Job run(String pluginID,String source,String direction,Listener listener) {
        return run(pluginID,source,direction,OpenAiSettings.disabled(),listener);
    }
    synchronized Job run(String pluginID,String source,String direction,OpenAiSettings.Snapshot profile,Listener listener) {
        if(listener==null) throw new IllegalArgumentException("Plugin listener is required");
        if(closed) throw new IllegalStateException("Plugin executor is closed");
        if(current!=null) current.cancel();
        Job job=new Job(listener); current=job;
        FutureTask<Void> task=new FutureTask<>(() -> { execute(job,pluginID,source,direction,profile); return null; });
        job.task=task; worker.execute(task); return job;
    }
    @Override public synchronized void close() {
        if(closed) return;
        closed=true; if(current!=null) { current.cancel(); current=null; }
        worker.shutdownNow(); worker.getQueue().clear();
    }
    final class Job {
        private final PluginCancellation cancellation=new PluginCancellation();
        private Listener listener;
        private volatile FutureTask<Void> task;
        private Job(Listener listener) { this.listener=listener; }
        void cancel() {
            synchronized(this) { cancellation.cancel(); listener=null; }
            FutureTask<Void> pending=task;
            if(pending!=null) { pending.cancel(true); worker.remove(pending); }
        }
        private synchronized void update(String text,boolean complete) {
            if(listener!=null && !cancellation.isCancelled()) listener.onUpdate(text,complete);
            if(complete) listener=null;
        }
        private synchronized void fail(String message) {
            if(listener!=null && !cancellation.isCancelled()) listener.onFailure(message);
            listener=null;
        }
        private synchronized void release() { listener=null; task=null; }
    }
    private void execute(Job job,String pluginID,String source,String direction,OpenAiSettings.Snapshot profile) {
        try {
            job.cancellation.check(); OpenAiChatCodec.validateSource(source);
            if(!packages.enabled(pluginID)) throw new IOException("请先在官方插件中安装并启用");
            if(profile.remote(pluginID)) {
                String instruction;
                try { instruction=packages.instruction(pluginID); }
                catch(Exception error) { throw new IOException("插件内容不可用",error); }
                byte[] request=OpenAiChatCodec.makeRemoteRequest(pluginID,source,profile.model,direction,instruction);
                String key=aiSettings.credential(profile);
                job.cancellation.check();
                OpenAiChatCodec.Decoder decoder=new OpenAiChatCodec.Decoder(job.cancellation,job::update);
                new HttpOpenAiTransport().stream(profile.endpoint(),request,key,job.cancellation,decoder::append);
                decoder.finish();
            } else if("translate".equals(pluginID)) {
                if(!"auto".equals(direction) && !"zh-en".equals(direction) && !"en-zh".equals(direction))
                    throw new OpenAiChatCodec.Failure(OpenAiChatCodec.Code.INVALID_DIRECTION,"请选择自动、中译英或英译中，原文已保留。");
                OfflineDictionary.Translation translated=dictionary.translate(source,direction,job.cancellation);
                job.cancellation.check();
                if(!translated.hasMatches()) throw new OpenAiChatCodec.Failure(OpenAiChatCodec.Code.DICTIONARY_UNCOVERED,"离线词典未覆盖这段内容，原文已保留。");
                String output=translated.fullyCovered()?translated.text:"【离线词典 · 部分匹配，未覆盖片段保留原文】\n"+translated.text;
                OpenAiChatCodec.validateUnicode(output);
                if(output.length()>OpenAiChatCodec.MAX_TEXT_UNITS)
                    throw new OpenAiChatCodec.Failure(OpenAiChatCodec.Code.OUTPUT_LIMIT,"翻译输出超过 16384 个 UTF-16 单元，原文已保留。");
                job.update(output,true);
            } else {
                byte[] request=OpenAiChatCodec.makeRequest(pluginID,source);
                OpenAiChatCodec.Decoder decoder=new OpenAiChatCodec.Decoder(job.cancellation,job::update);
                MockOpenAiTransport.LOCAL.stream(request,job.cancellation,decoder::append);
                decoder.finish();
            }
        } catch(PluginCancellation.Cancelled ignored) { /* Cancellation has no user-visible failure. */ }
        catch(OpenAiChatCodec.Failure error) { job.fail(error.getMessage()); }
        catch(IOException error) { job.fail("插件内容或离线词典无法读取，原文已保留。"); }
        catch(RuntimeException error) { job.fail("插件未完成，原文已保留。"); }
        finally { job.release(); }
    }
}
