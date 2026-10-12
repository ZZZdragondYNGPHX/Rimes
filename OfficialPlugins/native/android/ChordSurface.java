package org.scholay.rimes.android;

import android.content.Context;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import org.scholay.rimes.core.ChordGesture;
import org.scholay.rimes.core.ChordLayout;
import org.scholay.rimes.core.ChordProfile;

/** Multi-touch letter surface. Button accessibility and utility taps remain native Android paths. */
final class ChordSurface extends ViewGroup {
    interface Handler {
        void onChord(String code);
        void onKey(String text);
        void onPreview(ChordGesture.Preview preview);
        void onControl(ChordLayout.Action action);
        String label(ChordLayout.Action action);
        String description(ChordLayout.Action action);
        /** A held delete may generate its next action only while its host can accept it. */
        default boolean canRepeatDelete() { return true; }
    }
    private final Handler handler;
    private final ChordProfile profile=ChordProfile.builtIn();
    private final ChordGesture gesture=new ChordGesture(profile);
    private final Set<Integer> tracked=new HashSet<>();
    private final SparseArray<String> ordinary=new SparseArray<>();
    private List<ChordLayout.Key> frames;
    private boolean split,resolves=true,shifted;
    private boolean activeStream;
    private long streamDownTime=-1,retiredDownTime=-1;
    private String previewState="";
    private long ordinaryRevision,previewRevision=-1,previewOrdinaryRevision=-1,visualRevision=-1,visualOrdinaryRevision=-1;
    private boolean previewResolves,visualResolves,visualShifted;
    private KeyboardTheme visualTheme;
    private int visualUiMode=-1;
    private KeyboardTheme theme=KeyboardTheme.ALL[0];
    private int themeUiMode=-1;
    private Runnable utilityRetirementListener;
    private float verticalScale=1;
    ChordSurface(Context context,Handler handler) {
        super(context); this.handler=handler; setLayoutDirection(LAYOUT_DIRECTION_LTR);
        setMotionEventSplittingEnabled(false);
        frames=ChordLayout.keys(400,false);
        for(ChordLayout.Key key:frames) {
            KeyButton button=new KeyButton(context); button.classic(true); button.appearance(key.action!=ChordLayout.Action.TEXT,true,false);
            button.fontStyle(true,key.action==ChordLayout.Action.TEXT?21:17,true);
            button.setOnClickListener(view -> {
                if(gesture.active() || ordinary.size()>0) return;
                if(key.action==ChordLayout.Action.TEXT) handler.onKey(shifted?key.text.toUpperCase(java.util.Locale.ROOT):key.text);
                else handler.onControl(key.action);
            });
            addView(button);
        }
        updateButtons();
    }
    /** Semantic host attachment point; letter buttons remain owned by the chord gesture. */
    KeyButton utilityButton(ChordLayout.Action action) {
        if(action==null || action==ChordLayout.Action.TEXT) throw new IllegalArgumentException("A utility action is required");
        for(int i=0;i<frames.size();i++) if(frames.get(i).action==action) return (KeyButton)getChildAt(i);
        throw new IllegalArgumentException("Utility is not present in this chord layout");
    }
    /** A host's held utility action must retire whenever this surface loses touch authority. */
    void setUtilityRetirementListener(Runnable listener) {
        if(utilityRetirementListener==listener) return;
        if(utilityRetirementListener!=null) utilityRetirementListener.run();
        utilityRetirementListener=listener;
    }
    void render(boolean split,boolean resolves,boolean shifted,KeyboardTheme theme) {
        boolean geometryChanged=this.split!=split;
        int uiMode=getResources().getConfiguration().uiMode;
        boolean retire=geometryChanged || this.resolves!=resolves || this.shifted!=shifted || this.theme!=theme || themeUiMode!=uiMode;
        this.split=split; this.resolves=resolves; this.shifted=shifted; this.theme=theme;
        themeUiMode=uiMode;
        if(retire) cancel();
        if(geometryChanged) requestLayout();
        updateButtons();
    }
    /** Context loss retires pointer identities so delayed releases cannot submit to a new field. */
    void cancel() {
        if(utilityRetirementListener!=null) utilityRetirementListener.run();
        if(activeStream) retiredDownTime=streamDownTime;
        activeStream=false; streamDownTime=-1;
        gesture.reset(); tracked.clear(); ordinary.clear(); ordinaryRevision++;
        for(int i=0;i<getChildCount();i++) { getChildAt(i).setPressed(false); getChildAt(i).cancelPendingInputEvents(); }
        notifyPreview(); updateButtons();
    }
    boolean isChordActive() { return gesture.active() || ordinary.size()>0; }
    /** Busy work pauses held deletes without changing ordinary tap or accessibility ordering. */
    boolean canRepeatDelete() { return !isChordActive() && handler.canRepeatDelete(); }
    private float density() { return getResources().getDisplayMetrics().density; }
    private ChordLayout.Key keyAt(float x,float y) {
        if(verticalScale<=0) return null;
        float d=density(),px=x/d,py=y/(d*verticalScale);
        for(ChordLayout.Key key:frames) if(key.contains(px,py)) return key;
        return null;
    }
    private Character letterAt(MotionEvent event,int index) {
        ChordLayout.Key key=keyAt(event.getX(index),event.getY(index));
        return key!=null && key.action==ChordLayout.Action.TEXT?key.text.charAt(0):null;
    }
    @Override public boolean onInterceptTouchEvent(MotionEvent event) {
        if(event.getActionMasked()!=MotionEvent.ACTION_DOWN && event.getDownTime()==retiredDownTime) return true;
        if(!tracked.isEmpty() || ordinary.size()>0) return true;
        if(event.getActionMasked()!=MotionEvent.ACTION_DOWN) return false;
        return letterAt(event,event.getActionIndex())!=null;
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action=event.getActionMasked(),index=event.getActionIndex(),id=event.getPointerId(index);
        if(action==MotionEvent.ACTION_DOWN) {
            if(activeStream || !tracked.isEmpty() || ordinary.size()>0) cancel();
            activeStream=true; streamDownTime=event.getDownTime(); retiredDownTime=-1;
        } else if(!activeStream || event.getDownTime()!=streamDownTime) return true;
        if(action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN) {
            Character key=letterAt(event,index);
            if(key!=null) {
                performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                if(resolves) { tracked.add(id); gesture.begin(id,key); }
                else { ordinary.put(id,String.valueOf(key)); ordinaryRevision++; }
            } else if(resolves && !tracked.isEmpty()) {
                // An extra participating pointer on a gap/utility cannot silently join a valid batch.
                // Track its release too: cancellation stays quarantined until every finger lifts.
                tracked.add(id); gesture.begin(id,null);
            }
        } else if(action==MotionEvent.ACTION_MOVE) {
            for(int i=0;i<event.getPointerCount();i++) {
                int pointer=event.getPointerId(i); Character key=letterAt(event,i);
                if(resolves && tracked.contains(pointer)) gesture.move(pointer,key);
                else if(!resolves && ordinary.get(pointer)!=null && key!=null && ordinary.get(pointer).charAt(0)!=key) {
                    ordinary.put(pointer,String.valueOf(key)); ordinaryRevision++;
                }
            }
        } else if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {
            Character key=letterAt(event,index);
            if(resolves && tracked.remove(id)) {
                ChordProfile.Resolution result=gesture.end(id,key);
                if(!gesture.active() && result!=null) handler.onChord(result.input);
            } else if(!resolves) {
                String previous=ordinary.get(id);
                if(previous!=null) {
                    ordinary.remove(id); ordinaryRevision++; String text=key==null?previous:String.valueOf(key);
                    handler.onKey(shifted?text.toUpperCase(java.util.Locale.ROOT):text);
                }
            }
        } else if(action==MotionEvent.ACTION_CANCEL) cancel();
        if(action==MotionEvent.ACTION_UP) { activeStream=false; streamDownTime=-1; }
        notifyPreview(); updateButtons(); return true;
    }
    private void notifyPreview() {
        long revision=gesture.revision();
        if(previewRevision==revision && previewOrdinaryRevision==ordinaryRevision && previewResolves==resolves) return;
        previewRevision=revision; previewOrdinaryRevision=ordinaryRevision; previewResolves=resolves;
        ChordGesture.Preview preview=resolves?gesture.preview():null;
        String state=(isChordActive()?"1":"0")+"|"+(preview==null?"":
                (preview.left==null?"":preview.left.keys+":"+preview.left.output)+"|"
                +(preview.right==null?"":preview.right.keys+":"+preview.right.output)+"|"+preview.combined);
        // Touch sampling may move within one cap at 120 Hz. Identical readouts need no IME-wide render.
        if(!state.equals(previewState)) { previewState=state; handler.onPreview(preview); }
    }
    private void updateButtons() {
        long revision=gesture.revision(); int uiMode=getResources().getConfiguration().uiMode;
        if(visualRevision==revision && visualOrdinaryRevision==ordinaryRevision && visualTheme==theme && visualUiMode==uiMode
                && visualResolves==resolves && visualShifted==shifted) return;
        visualRevision=revision; visualOrdinaryRevision=ordinaryRevision; visualTheme=theme; visualUiMode=uiMode;
        visualResolves=resolves; visualShifted=shifted;
        int selected=gesture.keys(),eligible=gesture.availableKeys();
        for(int i=0;i<getChildCount();i++) {
            ChordLayout.Key key=frames.get(i); KeyButton button=(KeyButton)getChildAt(i);
            if(key.action==ChordLayout.Action.TEXT) {
                String label=key.text.toUpperCase(java.util.Locale.ROOT);
                if(!android.text.TextUtils.equals(button.getText(),label)) button.setText(label);
                button.setContentDescription(label);
                boolean pressed=(selected&ChordProfile.mask(key.text))!=0;
                for(int j=0;j<ordinary.size();j++) pressed|=key.text.equals(ordinary.valueAt(j));
                if(button.isPressed()!=pressed) button.setPressed(pressed);
                float alpha=resolves && gesture.active() && !pressed && (eligible&ChordProfile.mask(key.text))==0?0.3f:1;
                if(button.getAlpha()!=alpha) button.setAlpha(alpha);
            } else {
                button.icon(key.action==ChordLayout.Action.DELETE?KeyboardIcon.DELETE:KeyboardIcon.SMILE);
                String label=handler.label(key.action);
                if(!android.text.TextUtils.equals(button.getText(),label)) button.setText(label);
                button.setContentDescription(handler.description(key.action));
                button.setEnabled(!gesture.active() && ordinary.size()==0);
                float alpha=gesture.active()?0.3f:1; if(button.getAlpha()!=alpha) button.setAlpha(alpha);
            }
            button.theme(theme);
        }
    }
    @Override protected void onMeasure(int widthSpec,int heightSpec) {
        int width=MeasureSpec.getSize(widthSpec); float density=density();
        float logicalWidth=Math.max(1,width/density);
        int height=Math.round(ChordLayout.height(logicalWidth,split)*density);
        setMeasuredDimension(width,resolveSize(height,heightSpec));
        verticalScale=height<=0?0:Math.min(1,getMeasuredHeight()/(float)height);
        frames=ChordLayout.keys(logicalWidth,split);
        for(int i=0;i<frames.size();i++) {
            ChordLayout.Key key=frames.get(i);
            int w=Math.round((key.x+key.width)*density)-Math.round(key.x*density);
            int h=Math.round((key.y+key.height)*verticalScale*density)-Math.round(key.y*verticalScale*density);
            getChildAt(i).measure(MeasureSpec.makeMeasureSpec(w,MeasureSpec.EXACTLY),MeasureSpec.makeMeasureSpec(h,MeasureSpec.EXACTLY));
            ((KeyButton)getChildAt(i)).capFrame(0,0,w/density,h/density);
        }
    }
    @Override protected void onLayout(boolean changed,int l,int t,int r,int b) {
        if(changed) cancel();
        float density=density();
        for(int i=0;i<frames.size();i++) {
            ChordLayout.Key key=frames.get(i); android.view.View child=getChildAt(i);
            int x=Math.round(key.x*density),y=Math.round(key.y*verticalScale*density);
            child.layout(x,y,x+child.getMeasuredWidth(),y+child.getMeasuredHeight());
        }
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh) {
        super.onSizeChanged(w,h,oldw,oldh); if(w!=oldw || h!=oldh) cancel();
    }
    @Override protected void onDetachedFromWindow() { cancel(); super.onDetachedFromWindow(); }
    @Override public void onCancelPendingInputEvents() { super.onCancelPendingInputEvents(); if(gesture!=null) cancel(); }
    @Override protected void onVisibilityChanged(View changedView,int visibility) {
        super.onVisibilityChanged(changedView,visibility); if(visibility!=VISIBLE && gesture!=null) cancel();
    }
    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility); if(visibility!=VISIBLE && gesture!=null) cancel();
    }
}
