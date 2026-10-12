package org.scholay.rimes.core;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Two-thumb start/end gesture. A cancelled batch stays quarantined until all fingers lift. */
public final class ChordGesture {
    public static final class Side {
        public final String keys,output;
        Side(String keys,String output) { this.keys=keys; this.output=output; }
    }
    public static final class Preview {
        public final Side left,right;
        public final String combined;
        Preview(Side left,Side right,String combined) { this.left=left; this.right=right; this.combined=combined; }
    }
    private static final String[][] SLIDES={{"ty","ting"},{"tyu","tu"},{"gh","gang"},{"ghj","gan"},
            {"bn","bin"},{"bnm","bian"},{"bh","bang"},{"th","tang"},{"gy","guai"}};
    private static final class Contact {
        final int hand;
        final char start;
        final int startMask;
        char end;
        int endMask;
        boolean released;
        Contact(int hand,char start) { this.hand=hand; this.start=start; end=start; startMask=ChordProfile.mask(start); endMask=startMask; }
    }
    private final Map<Integer,Contact> contacts=new HashMap<>();
    private final Set<Integer> down=new HashSet<>();
    private final ChordProfile profile;
    private Contact[] contactList=new Contact[0];
    private String[] slide;
    private boolean cancelled;
    private long revision,availableRevision=-1,resolutionRevision=-1,previewRevision=-1;
    private int cachedAvailable;
    private ChordProfile.Resolution cachedResolution;
    private Preview cachedPreview;
    public ChordGesture(ChordProfile profile) { this.profile=profile; }
    public boolean active() { return !down.isEmpty(); }
    public boolean cancelled() { return cancelled; }
    /** Changes only when contact origins, endpoints, releases, cancellation or slide direction change. */
    public long revision() { return revision; }
    public int keys() {
        if(cancelled || contacts.isEmpty()) return 0;
        if(slide!=null) return ChordProfile.mask(slide[0]);
        int mask=0;
        for(Contact contact:contactList) mask|=contact.startMask|contact.endMask;
        return mask;
    }
    public ChordProfile.Resolution resolution() {
        if(resolutionRevision!=revision) {
            cachedResolution=slide!=null && !cancelled
                    ?new ChordProfile.Resolution(slide[0],slide[1],ChordProfile.syllableCode(slide[1])):profile.resolve(keys());
            resolutionRevision=revision;
        }
        return cachedResolution;
    }
    public Preview preview() {
        if(previewRevision==revision) return cachedPreview;
        previewRevision=revision;
        int keys=keys(); if(!active() || keys==0) { cachedPreview=null; return null; }
        int left=keys&profile.leftMask,right=keys&profile.rightMask;
        ChordProfile.Resolution result=resolution();
        cachedPreview=new Preview(side(left),side(right),result==null?null:result.preview); return cachedPreview;
    }
    private Side side(int keys) { return keys==0?null:new Side(profile.canonical(keys),profile.handOutput(keys)); }
    public int availableKeys() {
        if(availableRevision==revision) return cachedAvailable;
        cachedAvailable=computeAvailableKeys(); availableRevision=revision; return cachedAvailable;
    }
    private int computeAvailableKeys() {
        if(!active()) return profile.allMask;
        if(cancelled) return 0;
        int available=0;
        if(contacts.size()==1) {
            Contact contact=contactList[0];
            if(!contact.released) for(String[] path:SLIDES) if(path[0].charAt(0)==contact.start) available|=ChordProfile.mask(path[0]);
        }
        for(int keys:profile.combinations) {
            boolean valid=true;
            for(Contact contact:contactList) {
                int side=keys&profile.handMask(contact.hand);
                if((side&contact.startMask)==0 || contact.released && side!=(contact.startMask|contact.endMask)) { valid=false; break; }
            }
            if(valid) available|=keys;
        }
        return available;
    }
    public void begin(int id,Character key) {
        if(down.isEmpty()) reset();
        // A duplicate Android pointer ID cannot replace an existing contact.
        if(!down.add(id)) { cancel(); return; }
        revision++;
        if(slide!=null || cancelled || key==null || profile.hand(key)<0) { cancel(); return; }
        int hand=profile.hand(key);
        for(Contact contact:contactList) if(contact.hand==hand) { cancel(); return; }
        contacts.put(id,new Contact(hand,key));
        contactList=contacts.values().toArray(new Contact[0]);
    }
    public void move(int id,Character key) {
        Contact contact=contacts.get(id);
        if(cancelled || contact==null || contact.released) return;
        if(contacts.size()==1 && key!=null) {
            if(key==contact.start) { if(slide!=null) { slide=null; revision++; } }
            else {
                for(String[] route:SLIDES) if(route[0].charAt(0)==contact.start && route[0].charAt(route[0].length()-1)==key) {
                    if(slide!=route) { slide=route; revision++; } return;
                }
                if(slide!=null) return;
            }
        }
        // Gaps, blank ends and the other hand's region retain the last endpoint.
        if(key==null || profile.hand(key)!=contact.hand) return;
        if(key==contact.end) return;
        char previous=contact.end;
        int previousKeys=keys(),previousHand=contact.startMask|contact.endMask;
        contact.end=key; contact.endMask=ChordProfile.mask(key);
        int nextHand=contact.startMask|contact.endMask;
        boolean hadCombination=Integer.bitCount(previousKeys)>1 && profile.resolve(previousKeys)!=null;
        boolean hadHandCombination=Integer.bitCount(previousHand)>1 && profile.resolve(previousHand)!=null;
        if(key!=contact.start && profile.resolve(keys())==null
                && (hadCombination || hadHandCombination && profile.resolve(nextHand)==null)) {
            contact.end=previous; contact.endMask=ChordProfile.mask(previous);
        } else revision++;
    }
    public ChordProfile.Resolution end(int id,Character key) {
        if(!down.contains(id)) return null;
        move(id,key);
        Contact contact=contacts.get(id); if(contact!=null) contact.released=true;
        down.remove(id);
        revision++;
        if(!down.isEmpty()) return null;
        ChordProfile.Resolution result=resolution(); reset(); return result;
    }
    public void cancel() { if(!cancelled) { cancelled=true; revision++; } }
    public void reset() { contacts.clear(); contactList=new Contact[0]; down.clear(); slide=null; cancelled=false; revision++; }
}
