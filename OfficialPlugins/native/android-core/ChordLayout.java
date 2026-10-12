package org.scholay.rimes.core;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Pixel geometry port of iOS KeyboardGeometry: balanced orthogonal or split hand regions. */
public final class ChordLayout {
    public enum Action { TEXT, EMOJI, DELETE }
    public static final class Key {
        public final Action action;
        public final String text;
        public final float x,y,width,height;
        Key(Action action,String text,float x,float y,float width,float height) {
            this.action=action; this.text=text; this.x=x; this.y=y; this.width=width; this.height=height;
        }
        public boolean contains(float px,float py) { return px>=x && px<x+width && py>=y && py<y+height; }
    }
    private ChordLayout() {}
    public static float height(float width,boolean split) { return 3*Math.max(1,(Math.min(width,480)-(split?12:0))/10-1); }
    public static List<Key> keys(float width,boolean split) {
        if(width<=0) throw new IllegalArgumentException("Keyboard width must be positive");
        ChordProfile profile=ChordProfile.builtIn();
        String[] rows={"qwertyuiop","asdfghjkl","zxcvbnm,."};
        String[] hands={profile.leftKeys,profile.rightKeys};
        float gap=split?12:0,grid=Math.min(width,480),origin=(width-grid)/2,half=(grid-gap)/2,pitch=half/5,rowHeight=height(width,split)/3;
        List<Key> result=new ArrayList<>();
        for(int side=0;side<2;side++) for(int row=0;row<3;row++) {
            int column=0;
            for(char key:rows[row].toCharArray()) if(hands[side].indexOf(key)>=0) {
                result.add(new Key(Action.TEXT,String.valueOf(key),origin+side*(half+gap)+column*pitch+1,
                        row*rowHeight+0.5f,Math.max(1,pitch-2),Math.max(1,rowHeight-1))); column++;
            }
            if(side==1 && row>0) result.add(new Key(row==1?Action.EMOJI:Action.DELETE,"",
                    origin+half+gap+column*pitch+1,row*rowHeight+0.5f,Math.max(1,pitch-2),Math.max(1,rowHeight-1)));
        }
        return Collections.unmodifiableList(result);
    }
}
