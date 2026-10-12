package org.scholay.rimes.android;

import android.content.Context;
import android.graphics.Typeface;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Plugin settings explain the selected local or remote route. */
final class BufferPluginPanel extends ScrollView {
    interface Listener {
        void onPlugin(String id);
        void onDefaultBuffer();
        void onClose();
        void onDirection(String direction);
    }

    private final TextView title,notice;
    private final PluginShortcutBar shortcuts;
    private final KeyButton defaultBuffer,close;
    private final LinearLayout directions;
    private final KeyButton[] directionButtons=new KeyButton[3];
    private static final String[] DIRECTION_IDS={"auto","zh-en","en-zh"};
    private String displayedPlugin;
    private boolean rendered;

    BufferPluginPanel(Context context,Listener listener) {
        super(context);
        if(listener==null) throw new IllegalArgumentException("Plugin listener is required");
        setFillViewport(true); setVerticalScrollBarEnabled(false); setOverScrollMode(OVER_SCROLL_NEVER);
        LinearLayout column=new LinearLayout(context);
        column.setOrientation(LinearLayout.VERTICAL); column.setPadding(dp(12),dp(12),dp(12),dp(12));
        addView(column,new ScrollView.LayoutParams(LayoutParams.MATCH_PARENT,LayoutParams.WRAP_CONTENT));
        title=text(context,14,true); title.setText("Buffer 插件设置");
        column.addView(title,new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,LayoutParams.WRAP_CONTENT));
        shortcuts=new PluginShortcutBar(context,KeyboardTheme.ALL[0],new PluginShortcutBar.Listener() {
            @Override public void onPluginTap(String id) { listener.onPlugin(id); }
            @Override public void onPluginLongPress(String id) { listener.onPlugin(id); }
        });
        LinearLayout.LayoutParams selector=new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,dp(32));
        selector.topMargin=dp(8); column.addView(shortcuts,selector);
        notice=text(context,14,false); notice.setLineSpacing(dp(4),1); notice.setPadding(0,dp(8),0,dp(8));
        LinearLayout.LayoutParams message=new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,LayoutParams.WRAP_CONTENT);
        message.topMargin=dp(4); column.addView(notice,message);
        directions=new LinearLayout(context); directions.setOrientation(LinearLayout.HORIZONTAL);
        column.addView(directions,new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,dp(32)));
        String[] labels={"自动中英","中 → 英","英 → 中"};
        for(int i=0;i<labels.length;i++) {
            final String direction=DIRECTION_IDS[i];
            KeyButton value=button(context,labels[i],KeyboardIcon.TRANSLATE); value.setContentDescription("翻译方向："+labels[i]);
            value.setOnClickListener(view -> listener.onDirection(direction)); directionButtons[i]=value;
            LinearLayout.LayoutParams frame=new LinearLayout.LayoutParams(0,LayoutParams.MATCH_PARENT,1); frame.rightMargin=i==2?0:dp(4); directions.addView(value,frame);
        }
        LinearLayout actions=new LinearLayout(context); actions.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams actionRow=new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,dp(36));
        actionRow.topMargin=dp(8); column.addView(actions,actionRow);
        defaultBuffer=button(context,"普通 Buffer",KeyboardIcon.STACK_LAYERS);
        defaultBuffer.setContentDescription("返回普通 Buffer"); defaultBuffer.setOnClickListener(view -> listener.onDefaultBuffer());
        LinearLayout.LayoutParams first=new LinearLayout.LayoutParams(0,LayoutParams.MATCH_PARENT,1);
        first.rightMargin=dp(6); actions.addView(defaultBuffer,first);
        close=button(context,"返回键盘",KeyboardIcon.KEYBOARD);
        close.setContentDescription("关闭 Buffer 插件设置并返回键盘"); close.setOnClickListener(view -> listener.onClose());
        actions.addView(close,new LinearLayout.LayoutParams(0,LayoutParams.MATCH_PARENT,1));
        render(KeyboardTheme.ALL[0],null,"auto");
    }

    void render(KeyboardTheme theme,String pluginID,String direction) {
        KeyboardTheme.Palette palette=theme.palette(getContext());
        setBackgroundColor(palette.background); title.setTextColor(palette.ink); notice.setTextColor(palette.ink);
        shortcuts.render(theme,pluginID,true); defaultBuffer.theme(theme); close.theme(theme);
        directions.setVisibility("translate".equals(pluginID)?VISIBLE:GONE);
        for(int i=0;i<directionButtons.length;i++) { directionButtons[i].theme(theme); directionButtons[i].setSelected(DIRECTION_IDS[i].equals(direction)); }
        {
            OpenAiSettings.Snapshot profile=new OpenAiSettings(getContext()).snapshot();
            String name=pluginName(pluginID);
            notice.setText(name==null?"普通 Buffer 保留本次输入，确认发送后才进入输入框。"
                    :profile.remote(pluginID)?java.net.URI.create(profile.baseURL).getHost()+" · "+profile.model+"\n"+("translate".equals(pluginID)?"确认 Buffer 原文后自动发送给已配置的 AI；结果需手动发送。":"点执行后将本次 Buffer 原文发送给 AI；结果需确认发送。")
                    :"translate".equals(pluginID)?"本机中英词典查译，未覆盖词保留原文。\n逐词查译不保证句子语法；输入后自动本机查词，结果需手动发送。":name+"使用 OpenAI 格式本机 Mock。\n无网络请求；在 RIMES 主应用的 AI 服务中配置联网 AI。画画仅生成提示词。");
            displayedPlugin=pluginID;
            rendered=true;
        }
    }

    private static String pluginName(String id) {
        if("translate".equals(id)) return "翻译";
        if("ask".equals(id)) return "快问";
        if("polish".equals(id)) return "润色";
        if("poem".equals(id)) return "作诗";
        if("art".equals(id)) return "画画";
        return null;
    }
    private TextView text(Context context,int size,boolean medium) {
        TextView value=new TextView(context); value.setTextSize(size); value.setIncludeFontPadding(false);
        if(medium) value.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        return value;
    }
    private KeyButton button(Context context,String label,KeyboardIcon icon) {
        KeyButton value=new KeyButton(context); value.setText(label); value.fontStyle(false,14,true);
        value.appearance(true,true,true); value.icon(icon,16,true); value.setGravity(Gravity.CENTER);
        return value;
    }
    private int dp(float value) { return Math.round(value*getResources().getDisplayMetrics().density); }
}
