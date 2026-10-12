package org.scholay.rimes.core;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Immutable iOS built-in chord mappings, encoded into the existing Natural Code engine. */
public final class ChordProfile {
    private static final String ALPHABET="abcdefghijklmnopqrstuvwxyz,.";
    private static final Set<String> SYLLABLES=Collections.unmodifiableSet(new HashSet<>(Arrays.asList(ChordData.SYLLABLES.split(","))));
    private static final String[][] RULES={
        {"^([aoe])(ng)?$","$1$1$2"},{"iu$","Ⓠ"},{"[iu]a$","Ⓦ"},{"[uv]an$","Ⓡ"},
        {"[uv]e$","Ⓣ"},{"ing$|uai$","Ⓨ"},{"^sh","Ⓤ"},{"^ch","Ⓘ"},{"^zh","Ⓥ"},
        {"uo$","Ⓞ"},{"[uv]n$","Ⓟ"},{"(.)i?ong$","$1Ⓢ"},{"[iu]ang$","Ⓓ"},
        {"(.)en$","$1Ⓕ"},{"(.)eng$","$1Ⓖ"},{"(.)ang$","$1Ⓗ"},{"ian$","Ⓜ"},
        {"(.)an$","$1Ⓙ"},{"iao$","Ⓒ"},{"(.)ao$","$1Ⓚ"},{"(.)ai$","$1Ⓛ"},
        {"(.)ei$","$1Ⓩ"},{"ie$","Ⓧ"},{"ui$","Ⓥ"},{"(.)ou$","$1Ⓑ"},{"in$","Ⓝ"}
    };
    private static final String MARKERS="ⓆⓌⓇⓉⓎⓊⒾⓄⓅⓈⒹⒻⒼⒽⓂⒿⒸⓀⓁⓏⓍⓋⒷⓃ";
    private static final String CODES="qwrtyuiopsdfghmjcklzxvbn";
    private static final java.util.regex.Pattern[] PATTERNS=new java.util.regex.Pattern[RULES.length];
    private static final Map<String,String> FINALS;
    static {
        for(int i=0;i<RULES.length;i++) PATTERNS[i]=java.util.regex.Pattern.compile(RULES[i][0]);
        String[] names={"ai","ei","ao","ou","an","en","ang","eng","ong","ia","ie","iao","iu","ian","in","iang","ing","iong","ua","uo","uai","ui","uan","un","uang","ue","ve","van","vn"};
        String[] codes={"l","z","k","b","j","f","h","g","s","w","x","c","q","m","n","d","y","s","w","o","y","v","r","p","d","t","t","r","p"};
        Map<String,String> values=new HashMap<>();
        for(int i=0;i<names.length;i++) values.put(names[i],codes[i]);
        FINALS=Collections.unmodifiableMap(values);
    }
    public static final class Entry {
        public final String keys,output,input;
        public final boolean fragment;
        final int mask;
        Entry(String keys,String output,boolean fragment) {
            this.keys=keys; this.output=output; this.fragment=fragment; mask=mask(keys);
            input=fragment?fragmentCode(output):syllableCode(output);
            if(mask==0 || input==null) throw new IllegalArgumentException("Invalid bundled chord "+keys);
        }
    }
    public static final class Resolution {
        public final String keys,preview,input;
        Resolution(String keys,String preview,String input) { this.keys=keys; this.preview=preview; this.input=input; }
        @Override public boolean equals(Object other) {
            if(!(other instanceof Resolution)) return false;
            Resolution r=(Resolution)other;
            return keys.equals(r.keys) && preview.equals(r.preview) && input.equals(r.input);
        }
        @Override public int hashCode() { return java.util.Objects.hash(keys,preview,input); }
    }
    public final String leftKeys=ChordData.LEFT,rightKeys=ChordData.RIGHT;
    public final List<Entry> entries;
    public final int leftMask=mask(leftKeys),rightMask=mask(rightKeys),allMask=leftMask|rightMask;
    final int[] combinations;
    private final Map<Integer,Entry> mappings;
    private static final class BuiltIn { static final ChordProfile VALUE=new ChordProfile(); }
    public static ChordProfile builtIn() { return BuiltIn.VALUE; }
    private ChordProfile() {
        List<Entry> values=new ArrayList<>(); Map<Integer,Entry> indexed=new HashMap<>();
        for(String[] row:ChordData.MAPPINGS) {
            Entry entry=new Entry(row[0],row[1],row[2].equals("fragment"));
            if(indexed.put(entry.mask,entry)!=null || (entry.mask&~allMask)!=0
                    || Integer.bitCount(entry.mask&leftMask)>2 || Integer.bitCount(entry.mask&rightMask)>2)
                throw new IllegalArgumentException("Invalid bundled chord "+entry.keys);
            values.add(entry);
        }
        entries=Collections.unmodifiableList(values); mappings=Collections.unmodifiableMap(indexed);
        Set<Integer> reachable=new LinkedHashSet<>();
        for(int i=0;i<ALPHABET.length();i++) if((allMask&(1<<i))!=0) reachable.add(1<<i);
        reachable.addAll(indexed.keySet());
        List<Integer> left=fragments(leftMask),right=fragments(rightMask);
        for(int a:left) for(int b:right) if(resolve(a|b)!=null) reachable.add(a|b);
        combinations=new int[reachable.size()]; int i=0;
        for(int keys:reachable) combinations[i++]=keys;
    }
    private List<Integer> fragments(int hand) {
        List<Integer> values=new ArrayList<>();
        for(int i=0;i<ALPHABET.length();i++) if((hand&(1<<i))!=0) values.add(1<<i);
        for(Entry entry:entries) if(entry.fragment && (entry.mask&~hand)==0) values.add(entry.mask);
        return values;
    }
    public static int mask(String keys) {
        if(keys==null) return 0; int result=0;
        for(int i=0;i<keys.length();i++) { int index=ALPHABET.indexOf(keys.charAt(i)); if(index<0) return 0; result|=1<<index; }
        return result;
    }
    public static int mask(char key) { int index=ALPHABET.indexOf(key); return index<0?0:1<<index; }
    public int hand(char key) { int bit=mask(key); return (bit&leftMask)!=0?0:(bit&rightMask)!=0?1:-1; }
    public int handMask(int hand) { return hand==0?leftMask:rightMask; }
    public String canonical(int keys) {
        StringBuilder result=new StringBuilder();
        for(char key:(leftKeys+rightKeys).toCharArray()) if((mask(key)&keys)!=0) result.append(key);
        return result.toString();
    }
    public Resolution resolve(String keys) { return resolve(mask(keys)); }
    public Resolution resolve(int keys) {
        if(keys==0 || (keys&~allMask)!=0) return null;
        String canonical=canonical(keys);
        if(Integer.bitCount(keys)==1) return new Resolution(canonical,canonical,canonical);
        Entry entry=mappings.get(keys);
        if(entry!=null) return new Resolution(canonical,entry.output,entry.input);
        int left=keys&leftMask,right=keys&rightMask;
        if(left==0 || right==0) return null;
        String a=fragment(left),b=fragment(right);
        if(a==null || b==null) return null;
        String combined=a+b,code=syllableCode(combined);
        return code==null?null:new Resolution(canonical,combined,code);
    }
    public String handOutput(int keys) {
        if(keys==0) return null;
        if(Integer.bitCount(keys)==1) return canonical(keys);
        Entry entry=mappings.get(keys); return entry==null?null:entry.output;
    }
    private String fragment(int keys) {
        if(Integer.bitCount(keys)==1) return canonical(keys);
        Entry entry=mappings.get(keys); return entry!=null && entry.fragment?entry.output:null;
    }
    public static String syllableCode(String pinyin) {
        if(!SYLLABLES.contains(pinyin)) return null;
        String text=pinyin;
        for(int i=0;i<RULES.length;i++) text=PATTERNS[i].matcher(text).replaceAll(RULES[i][1]);
        StringBuilder code=new StringBuilder();
        for(char letter:text.toCharArray()) { int index=MARKERS.indexOf(letter); code.append(index<0?letter:CODES.charAt(index)); }
        return code.length()==2?code.toString():null;
    }
    public static String fragmentCode(String pinyin) {
        if(pinyin.equals("zh")) return "v";
        if(pinyin.equals("ch")) return "i";
        if(pinyin.equals("sh")) return "u";
        if(pinyin.length()==1 && "abcdefghijklmnopqrstuvwxyz".contains(pinyin)) return pinyin;
        return FINALS.get(pinyin);
    }
}
