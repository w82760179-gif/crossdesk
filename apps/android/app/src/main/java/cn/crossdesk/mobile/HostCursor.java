package cn.crossdesk.mobile;

import android.graphics.Bitmap;
import org.json.JSONObject;

final class HostCursor {
    int shape, hotX, hotY, desktopWidth;
    boolean visible = true;
    Bitmap bitmap;
    private boolean received;
    private int sequence;
    private String previousRuns = "";

    boolean update(JSONObject state) {
        if (state == null || !state.has("seq")) return false;
        int seq=(int)state.optLong("seq");
        if(received && seq-sequence<=0)return false;
        sequence=seq; received=true;
        int mode=state.optInt("render_mode",0);
        if(mode==4)return false; // Unknown sample: retain the most recent appearance.
        int next=state.optInt("shape",0);
        boolean show=mode!=2 && mode!=3 && state.optBoolean("visible",true) && next!=1;
        boolean changed=shape!=next || visible!=show;
        if(shape!=next || !show){bitmap=null;previousRuns="";}
        shape=next;visible=show;
        JSONObject pixels=state.optJSONObject("pixels");
        if(!show || pixels==null)return changed;
        int w=pixels.optInt("w"), h=pixels.optInt("h");
        int hx=pixels.optInt("hx",-1),hy=pixels.optInt("hy",-1),dw=pixels.optInt("desktop_w");
        String runs=pixels.optString("runs","");
        if(w<1||w>128||h<1||h>128||hx<0||hx>=w||hy<0||hy>=h||dw<1||
           runs.length()>11200||runs.length()%10!=0)return changed;
        if(bitmap!=null && w==bitmap.getWidth() && h==bitmap.getHeight() && hx==hotX &&
           hy==hotY && dw==desktopWidth && runs.equals(previousRuns))return changed;
        int[] colors=new int[w*h];
        int offset=0;
        try {
            for(int i=0;i<runs.length();i+=10){
                int count=Integer.parseInt(runs.substring(i,i+2),16);
                int color=(int)Long.parseLong(runs.substring(i+2,i+10),16);
                if(count<=0||count>colors.length-offset)return changed;
                java.util.Arrays.fill(colors,offset,offset+count,color);
                offset+=count;
            }
        }catch(NumberFormatException invalid){return changed;}
        if(offset!=colors.length)return changed;
        bitmap=Bitmap.createBitmap(colors,w,h,Bitmap.Config.ARGB_8888);
        hotX=hx;hotY=hy;desktopWidth=dw;previousRuns=runs;
        return true;
    }
}
