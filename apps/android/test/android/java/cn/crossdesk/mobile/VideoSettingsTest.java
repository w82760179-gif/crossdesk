package cn.crossdesk.mobile;

import org.junit.Test;
import static org.junit.Assert.*;

public class VideoSettingsTest {
    private final RemoteVideoSettings.Values first=new RemoteVideoSettings.Values(1,30,2),second=new RemoteVideoSettings.Values(0,60,1);
    private void same(RemoteVideoSettings.Values expected,RemoteVideoSettings.Values actual){assertEquals(expected.quality,actual.quality);assertEquals(expected.frameRate,actual.frameRate);assertEquals(expected.preference,actual.preference);}

    @Test public void officeDefaultMaintainsResolutionAndHonorsFrameRatePreference(){
        RemoteVideoSettings state=new RemoteVideoSettings(RemoteVideoSettings.DEFAULT_PREFERENCE,3000);
        same(new RemoteVideoSettings.Values(1,90,1),state.selection);
        same(new RemoteVideoSettings.Values(1,90,0),new RemoteVideoSettings(0,3000).selection);
        same(new RemoteVideoSettings.Values(1,90,1),new RemoteVideoSettings(1,3000).selection);
        same(new RemoteVideoSettings.Values(0,60,1),new RemoteVideoSettings(-1,1280).selection);
        assertTrue(new RemoteVideoSettings.Values(1,90,1).valid());
    }
    @Test public void acknowledgementCommitsAndRejectionRollsBack(){
        RemoteVideoSettings state=new RemoteVideoSettings(1);state.setSupported(true);long id=state.begin(first);assertTrue(state.pending);same(new RemoteVideoSettings.Values(1,90,1),state.applied);
        state.receive(first,id,true);same(first,state.selection);assertFalse(state.pending);assertFalse(state.failed);state.expire(id);assertFalse(state.failed);
        id=state.begin(second);state.receive(second,id,false);same(first,state.selection);assertTrue(state.failed);state.begin(second);assertFalse(state.failed);
    }

    @Test public void timeoutLateReplyAndCapabilityLoss(){
        RemoteVideoSettings state=new RemoteVideoSettings(1);state.setSupported(true);long id=state.begin(first);state.expire(id);assertTrue(state.failed);same(new RemoteVideoSettings.Values(1,90,1),state.selection);
        state.receive(first,id,true);same(first,state.selection);assertFalse(state.failed);
        id=state.begin(second);state.setSupported(false);state.receive(second,id,true);state.expire(id);same(first,state.selection);assertFalse(state.failed);assertFalse(state.pending);
    }

}
