package org.scholay.rimes.android;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Looper;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.GZIPInputStream;

/** Pinned CC-CEDICT word/phrase lookup. It does not generate contextual sentences. */
final class OfflineDictionary {
    static final int TEXT_LIMIT=16384;
    // AAPT transparently decompresses/renames .gz assets. A neutral extension
    // keeps these exact gzip bytes so verification and runtime decoding agree.
    static final String ASSET="dictionary/cedict-index.rmdict";
    private static final byte[] MAGIC={'R','M','D','I','C','T','0','3'};
    private static final Object LOAD_LOCK=new Object();
    // Only immutable dictionary data is shared. No source, result or input history is cached.
    private static volatile Index sharedIndex;
    private final AssetManager assets;

    OfflineDictionary(Context context) {
        assets=context.getApplicationContext().getAssets();
    }

    /** Counts lexical segments; punctuation, whitespace and stand-alone numbers are neutral. */
    static final class Translation {
        public final String text,resolvedDirection;
        public final int matchedUnits,unknownUnits;
        Translation(String text,String direction,int matched,int unknown) {
            this.text=text; resolvedDirection=direction; matchedUnits=matched; unknownUnits=unknown;
        }
        public boolean hasMatches() { return matchedUnits>0; }
        public boolean fullyCovered() { return hasMatches() && unknownUnits==0; }
    }

    /** Call on a worker. The first invocation loads the precompiled asset, cooperatively. */
    Translation translate(String source,String direction,PluginCancellation cancellation) throws IOException {
        if(Looper.myLooper()==Looper.getMainLooper()) throw new IllegalStateException("Dictionary lookup requires a worker thread");
        if(source==null || source.length()>TEXT_LIMIT) throw new IllegalArgumentException("Dictionary source exceeds its text limit");
        if(cancellation==null) throw new IllegalArgumentException("Dictionary cancellation is required");
        cancellation.check();
        String resolved=resolveDirection(source,direction);
        if(source.isEmpty()) return new Translation("",resolved,0,0);
        Index index=load(cancellation);
        Trie trie=resolved.equals("zh-en")?index.chinese:index.english;
        StringBuilder result=new StringBuilder(Math.min(TEXT_LIMIT,source.length()+32));
        int matched=0,unknown=0,position=0;
        boolean previousTranslated=false;
        while(position<source.length()) {
            cancellation.check();
            int cp=source.codePointAt(position),width=Character.charCount(cp);
            boolean eligible=resolved.equals("zh-en")?isHan(cp):isEnglishLetter(cp);
            long match=eligible?trie.longest(source,position,resolved.equals("en-zh")):0;
            if(match!=0) {
                int end=(int)(match>>>32),value=(int)match-1;
                String gloss=trie.value(value);
                if(previousTranslated && resolved.equals("zh-en") && result.length()>0
                        && isEnglishLetter(result.charAt(result.length()-1)) && !gloss.isEmpty()
                        && isEnglishLetter(gloss.charAt(0))) append(result," ");
                append(result,gloss);
                position=end; matched++; previousTranslated=true;
            } else {
                int end=position+width;
                boolean unknownWord=Character.isLetter(cp);
                if(!isHan(cp) && (Character.isLetterOrDigit(cp) || cp=='_')) {
                    // Preserve unknown identifiers/words as one segment, without partial
                    // single-letter dictionary matches inside them.
                    while(end<source.length()) {
                        int next=source.codePointAt(end);
                        if(!isWordPart(next) || isHan(next)) break;
                        unknownWord|=Character.isLetter(next);
                        end+=Character.charCount(next);
                    }
                }
                append(result,source,position,end);
                if(!isNeutral(cp) || unknownWord) unknown++;
                position=end; previousTranslated=false;
            }
        }
        cancellation.check();
        return new Translation(result.toString(),resolved,matched,unknown);
    }

    private static String resolveDirection(String source,String direction) {
        if(direction==null || direction.equals("auto")) {
            for(int i=0;i<source.length();) {
                int cp=source.codePointAt(i);
                if(isHan(cp)) return "zh-en";
                i+=Character.charCount(cp);
            }
            return "en-zh";
        }
        if(direction.equals("zh-en") || direction.equals("en-zh")) return direction;
        throw new IllegalArgumentException("Unsupported dictionary direction");
    }

    private static boolean isHan(int cp) { return Character.UnicodeScript.of(cp)==Character.UnicodeScript.HAN; }
    private static boolean isEnglishLetter(int cp) { return cp>='a' && cp<='z' || cp>='A' && cp<='Z'; }
    private static boolean isWordPart(int cp) { return Character.isLetterOrDigit(cp) || cp=='\'' || cp=='\u2019' || cp=='-' || cp=='_'; }
    private static boolean isEnglishContinuation(String source,int position) {
        if(position==source.length()) return false;
        int cp=source.codePointAt(position);
        if(Character.isLetterOrDigit(cp)) return !isHan(cp);
        if(cp=='_') return true;
        int next=position+Character.charCount(cp);
        return (cp=='\'' || cp=='\u2019' || cp=='-') && next<source.length()
                && Character.isLetterOrDigit(source.codePointAt(next)) && !isHan(source.codePointAt(next));
    }
    private static boolean isNeutral(int cp) {
        if(Character.isWhitespace(cp) || Character.isSpaceChar(cp) || Character.isDigit(cp)) return true;
        int type=Character.getType(cp);
        return type==Character.CONNECTOR_PUNCTUATION || type==Character.DASH_PUNCTUATION
                || type==Character.START_PUNCTUATION || type==Character.END_PUNCTUATION
                || type==Character.INITIAL_QUOTE_PUNCTUATION || type==Character.FINAL_QUOTE_PUNCTUATION
                || type==Character.OTHER_PUNCTUATION;
    }

    private static void append(StringBuilder output,String value) throws IOException {
        if(value.length()>TEXT_LIMIT-output.length()) throw new IOException("Dictionary output exceeds its text limit");
        output.append(value);
    }
    private static void append(StringBuilder output,String source,int start,int end) throws IOException {
        if(end-start>TEXT_LIMIT-output.length()) throw new IOException("Dictionary output exceeds its text limit");
        output.append(source,start,end);
    }

    private Index load(PluginCancellation cancellation) throws IOException {
        Index loaded=sharedIndex;
        if(loaded!=null) return loaded;
        synchronized(LOAD_LOCK) {
            cancellation.check();
            loaded=sharedIndex;
            if(loaded!=null) return loaded;
            try(DataInputStream input=new DataInputStream(new BufferedInputStream(
                    new GZIPInputStream(assets.open(ASSET),32768),32768))) {
                byte[] magic=new byte[MAGIC.length]; input.readFully(magic);
                if(!Arrays.equals(magic,MAGIC) || input.readInt()!=3) throw new IOException("Unsupported dictionary index");
                int sourceEntries=input.readInt();
                if(sourceEntries<=0 || sourceEntries>1000000) throw new IOException("Invalid dictionary entry count");
                Budget budget=new Budget();
                Trie chinese=Trie.read(input,budget,cancellation),english=Trie.read(input,budget,cancellation);
                if(input.read()!=-1) throw new IOException("Trailing dictionary index data");
                cancellation.check();
                loaded=new Index(chinese,english);
                sharedIndex=loaded;
                return loaded;
            }
        }
    }

    private static final class Index {
        final Trie chinese,english;
        Index(Trie chinese,Trie english) { this.chinese=chinese; this.english=english; }
    }
    private static final class Budget {
        long bytes;
        void reserve(long count) throws IOException {
            bytes+=count;
            if(bytes>32L*1024*1024) throw new IOException("Dictionary index exceeds its memory limit");
        }
    }

    /** Sorted radix edges and byte pools avoid per-entry HashMaps/String objects. */
    private static final class Trie {
        final int[] firstEdge,terminal,labelOffsets,targets,valueOffsets;
        final char[] labels;
        final byte[] values;
        Trie(int[] first,int[] terminal,int[] offsets,int[] targets,char[] labels,int[] valueOffsets,byte[] values) {
            this.firstEdge=first; this.terminal=terminal; labelOffsets=offsets;
            this.targets=targets; this.labels=labels; this.valueOffsets=valueOffsets; this.values=values;
        }

        String value(int index) {
            int start=valueOffsets[index];
            return new String(values,start,valueOffsets[index+1]-start,StandardCharsets.UTF_8);
        }

        long longest(String source,int start,boolean english) {
            int node=0,position=start;
            long best=0;
            while(position<source.length()) {
                char wanted=normalized(source.charAt(position),english);
                int low=firstEdge[node],high=firstEdge[node+1]-1,edge=-1;
                while(low<=high) {
                    int middle=(low+high)>>>1;
                    char first=labels[labelOffsets[middle]];
                    if(first<wanted) low=middle+1;
                    else if(first>wanted) high=middle-1;
                    else { edge=middle; break; }
                }
                if(edge<0) break;
                int cursor=position;
                boolean equal=true;
                for(int unit=labelOffsets[edge];unit<labelOffsets[edge+1];unit++) {
                    if(cursor==source.length() || labels[unit]!=normalized(source.charAt(cursor),english)) {
                        equal=false; break;
                    }
                    cursor++;
                }
                if(!equal) break;
                position=cursor; node=targets[edge];
                if(terminal[node]>=0 && (!english || !isEnglishContinuation(source,position)))
                    best=((long)position<<32) | ((long)terminal[node]+1);
            }
            return best;
        }
        private static char normalized(char value,boolean english) {
            return english && value>='A' && value<='Z'?(char)(value+'a'-'A'):value;
        }

        static Trie read(DataInputStream input,Budget budget,PluginCancellation cancellation) throws IOException {
            int nodes=count(input,1000000),edges=count(input,1000000),labelUnits=count(input,2000000);
            int valueCount=count(input,300000),valueBytes=count(input,10000000);
            if(nodes<=0 || edges!=nodes-1 || valueCount<=0) throw new IOException("Invalid dictionary trie counts");
            budget.reserve(4L*(nodes+1+nodes+edges+1+edges+valueCount+1)+2L*labelUnits+valueBytes);
            int[] first=ints(input,nodes+1,cancellation),terminal=ints(input,nodes,cancellation);
            int[] offsets=ints(input,edges+1,cancellation),targets=ints(input,edges,cancellation);
            char[] labels=new char[labelUnits];
            for(int i=0;i<labelUnits;i++) { if((i&1023)==0) cancellation.check(); labels[i]=input.readChar(); }
            int[] valueOffsets=ints(input,valueCount+1,cancellation);
            byte[] values=new byte[valueBytes];
            for(int offset=0;offset<valueBytes;) {
                cancellation.check(); int length=Math.min(32768,valueBytes-offset);
                input.readFully(values,offset,length); offset+=length;
            }
            if(first[0]!=0 || first[nodes]!=edges || offsets[0]!=0 || offsets[edges]!=labelUnits
                    || valueOffsets[0]!=0 || valueOffsets[valueCount]!=valueBytes) throw new IOException("Invalid dictionary index bounds");
            for(int node=0;node<nodes;node++) {
                if((node&1023)==0) cancellation.check();
                if(first[node]<0 || first[node+1]<first[node] || first[node+1]>edges
                        || terminal[node]<-1 || terminal[node]>=valueCount) throw new IOException("Invalid dictionary node");
                int previous=-1;
                for(int edge=first[node];edge<first[node+1];edge++) {
                    if(offsets[edge]<0 || offsets[edge+1]<=offsets[edge] || offsets[edge+1]>labelUnits
                            || targets[edge]<=node || targets[edge]>=nodes) throw new IOException("Invalid dictionary edge");
                    int head=labels[offsets[edge]];
                    if(head<=previous) throw new IOException("Unsorted dictionary edges");
                    previous=head;
                }
            }
            for(int value=0;value<valueCount;value++) {
                if((value&1023)==0) cancellation.check();
                if(valueOffsets[value]<0 || valueOffsets[value+1]<=valueOffsets[value]
                        || valueOffsets[value+1]>valueBytes) throw new IOException("Invalid dictionary value bounds");
            }
            return new Trie(first,terminal,offsets,targets,labels,valueOffsets,values);
        }
        private static int count(DataInputStream input,int maximum) throws IOException {
            int value=input.readInt();
            if(value<0 || value>maximum) throw new IOException("Invalid dictionary allocation count");
            return value;
        }
        private static int[] ints(DataInputStream input,int count,PluginCancellation cancellation) throws IOException {
            int[] result=new int[count];
            for(int i=0;i<count;i++) { if((i&1023)==0) cancellation.check(); result[i]=input.readInt(); }
            return result;
        }
    }
}
