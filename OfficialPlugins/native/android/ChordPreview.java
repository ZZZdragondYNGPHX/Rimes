package org.scholay.rimes.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.view.View;
import java.util.Locale;
import org.scholay.rimes.core.ChordGesture;

/** Port of iOS ChordHandPreviewView: both hands pack outward from a centered result pill. */
final class ChordPreview extends View {
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF pill=new RectF();
    private final Typeface keyFace,outputFace,combinedFace;
    private final String[] texts={"","—","","—",""};
    private final int[] colors=new int[5];
    private final float[] sizes=new float[5];
    private ChordGesture.Preview preview;
    private KeyboardTheme.Palette palette;
    ChordPreview(Context context) {
        super(context); setClickable(false);
        keyFace=weight(Typeface.MONOSPACE,500);
        outputFace=weight(Typeface.create("sans-serif",Typeface.NORMAL),600);
        combinedFace=weight(Typeface.create("sans-serif",Typeface.NORMAL),700);
    }
    private static Typeface weight(Typeface face,int weight) {
        return Build.VERSION.SDK_INT>=28?Typeface.create(face,weight,false):face;
    }
    void render(ChordGesture.Preview preview,KeyboardTheme theme,boolean landscape) {
        if(preview==null) { clear(); return; }
        this.preview=preview; palette=theme.palette(getContext());
        side(preview==null?null:preview.left,0,1);
        side(preview==null?null:preview.right,4,3);
        boolean mapped=preview!=null && preview.combined!=null;
        texts[2]=preview==null?"":mapped?preview.combined:"无映射";
        colors[2]=mapped?palette.accentInk:withAlpha(palette.ink,153);
        sizes[0]=sizes[4]=landscape?12:13;
        sizes[1]=sizes[3]=landscape?15:17;
        sizes[2]=mapped?(landscape?20:22):13;
        setContentDescription(preview==null?null:String.join(" ",texts));
        invalidate();
    }
    void clear() {
        if(preview==null) return;
        preview=null; java.util.Arrays.fill(texts,"");
        setContentDescription(null); invalidate();
    }
    private void side(ChordGesture.Side side,int keys,int output) {
        texts[keys]=side==null?"":side.keys.toUpperCase(Locale.ROOT);
        texts[output]=side==null?"—":side.output==null?"?":side.output;
        colors[keys]=withAlpha(palette.ink,153);
        colors[output]=side==null?withAlpha(palette.ink,102):side.output==null?0xffff453a:palette.accentText;
    }
    private static int withAlpha(int color,int alpha) { return (color&0xffffff)|(alpha<<24); }
    private float density() { return getResources().getDisplayMetrics().density; }
    private void configure(int column) {
        paint.setColor(colors[column]); paint.setTextSize(sizes[column]*getResources().getDisplayMetrics().scaledDensity);
        paint.setTypeface(column==0 || column==4?keyFace:column==2 && preview.combined!=null?combinedFace:outputFace);
    }
    private float natural(int column) { configure(column); return texts[column].isEmpty()?0:(float)Math.ceil(paint.measureText(texts[column])); }
    private void text(Canvas canvas,int column,float center,float width) {
        if(width<=0 || texts[column].isEmpty()) return;
        configure(column); float actual=paint.measureText(texts[column]);
        if(actual>width) paint.setTextSize(paint.getTextSize()*Math.max(0.5f,width/actual));
        Paint.FontMetrics metrics=paint.getFontMetrics();
        canvas.drawText(texts[column],center-paint.measureText(texts[column])/2,
                getHeight()/2f-(metrics.ascent+metrics.descent)/2,paint);
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas); if(preview==null || palette==null) return;
        float d=density(),mid=getWidth()/2f,pillWidth=Math.min(getWidth()*0.42f,Math.max(58*d,natural(2)+20*d));
        pill.set(mid-pillWidth/2,2*d,mid+pillWidth/2,Math.max(2*d,getHeight()-2*d));
        paint.setColor(preview.combined!=null?palette.accent:withAlpha(palette.ink,30));
        canvas.drawRoundRect(pill,8*d,8*d,paint); text(canvas,2,mid,pillWidth-12*d);
        float room=Math.max(0,(getWidth()-pillWidth)/2-4*d);
        for(int sign=-1;sign<=1;sign+=2) {
            int output=sign<0?1:3,keys=sign<0?0:4;
            float outputWidth=Math.min(natural(output),room*0.55f),keysWidth=Math.min(natural(keys),Math.max(0,room-outputWidth-14*d));
            float outputCenter=mid+sign*(pillWidth/2+8*d+outputWidth/2);
            text(canvas,output,outputCenter,outputWidth);
            text(canvas,keys,outputCenter+sign*(outputWidth/2+6*d+keysWidth/2),keysWidth);
        }
    }
}
