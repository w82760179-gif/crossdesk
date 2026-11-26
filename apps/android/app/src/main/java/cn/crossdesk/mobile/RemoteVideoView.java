package cn.crossdesk.mobile;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.PixelCopy;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

@android.annotation.SuppressLint("ViewConstructor") // Constructed only with a live session, never from XML.
final class RemoteVideoView extends FrameLayout implements SurfaceHolder.Callback {
    private final SurfaceView video;
    private final Cursor cursor;
    private final NativeSession session;
    final RemoteMouseInput mouseInput;
    Runnable remoteFocusChanged;
    private final GestureDetector gestures;
    private final ScaleGestureDetector scales;
    private final RemoteViewport viewport = new RemoteViewport();
    private final RemoteViewport pictureInPictureViewport = new RemoteViewport();
    private boolean pictureInPicture;
    private int frameWidth = 16, frameHeight = 9;
    private int targetFrameRate = 90;
    private float x = .5f, y = .5f, scrollY;
    private boolean dragging, twoFinger, relative, viewportGesture, viewportPanActive;
    private float lastX,lastY,twoFingerX,twoFingerY;
    private long touchRevision;
    private boolean suppressPointerGesture;
    private boolean captureMouse = true;
    private boolean tabletTouch = true, tabletTouchActive;
    private int hardwareButtons;
    private final HostCursor host = new HostCursor();
    private final int[] controlsFrom = new int[2], controlsTo = new int[2];
    private boolean implicitPrimary;
    private boolean tapConsumed, directTap;
    void hostCursor(org.json.JSONObject state) {
        if (host.update(state)) cursor.postInvalidateOnAnimation();
    }
    boolean touchFromControls(View source, MotionEvent event) {
        source.getLocationOnScreen(controlsFrom); getLocationOnScreen(controlsTo);
        MotionEvent local=MotionEvent.obtain(event);
        local.offsetLocation(controlsFrom[0]-controlsTo[0],controlsFrom[1]-controlsTo[1]);
        try { return touch(local); } finally { local.recycle(); }
    }
    void setRelative(boolean value) { releaseMouse(); relative=value; if (!value) releasePointerCaptureSafe(); }
    void setFillScreen(boolean value) {
        if (viewport.fillScreen() == value) return;
        viewport.setFillScreen(value);
        viewport.reset();
        fit();
    }
    void setCaptureMouse(boolean value) {
        captureMouse = value;
        if (!value) releasePointerCaptureSafe();
    }
    void setTabletTouch(boolean value) {
        if (tabletTouch == value) return;
        tabletTouch = value;
        if (!value && tabletTouchActive) {
            tabletTouchActive = false;
            session.touch(new int[0], new int[0], new float[0], new float[0], 0);
        }
    }
    void releasePointerCaptureSafe() {
        implicitPrimary=false; updateHardwareButtons(0);
        if (android.os.Build.VERSION.SDK_INT >= 26 && hasPointerCapture()) {
            releasePointerCapture();
        }
    }
    void setPictureInPicture(boolean value) {
        if(pictureInPicture==value)return;
        releaseMouse();if(value)releasePointerCaptureSafe();pictureInPicture=value;cursor.setVisibility(value?GONE:VISIBLE);fit();
    }
    Rect pictureInPictureBounds() {
        Rect bounds=new Rect();video.getGlobalVisibleRect(bounds);return bounds;
    }
    void releaseMouse() { updateHardwareButtons(0); mouseInput.releaseAll(); dragging=false; suppressPointerGesture=true; }
    private boolean suppressPointerGesture() {
        return suppressPointerGesture || mouseInput.hasVirtualButton() || touchRevision != mouseInput.revision();
    }
    RemoteVideoView(Context context, NativeSession session) {
        super(context); this.session = session;
        mouseInput = new RemoteMouseInput(this::sendMouse);
        mouseInput.relativeMotion = this::moveRelative;
        setBackgroundColor(Color.BLACK);
        setFocusable(true);
        setFocusableInTouchMode(true);
        video = new SurfaceView(context); video.getHolder().addCallback(this);
        addView(video, new LayoutParams(1, 1, android.view.Gravity.CENTER));
        cursor = new Cursor(context);
        // Keep input in container coordinates so transforms never move its hit area.
        addView(cursor, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        gestures = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return point(e); }
            @Override public boolean onSingleTapUp(MotionEvent e) {
                if (!directTap && !twoFinger && !suppressPointerGesture() && point(e)) cursor.performClick(); return true;
            }
            @Override public void onLongPress(MotionEvent e) { if (!twoFinger && !dragging && !suppressPointerGesture() && point(e)) { tapConsumed=true; click(3, 4); } }
            @Override public boolean onScroll(MotionEvent first, MotionEvent current, float dx, float dy) {
                if (twoFinger) return true;
                if (!relative && !dragging && !suppressPointerGesture()) {
                    if (!point(first)) return true;
                    mouseInput.gestureLeft(true); dragging = true;
                }
                point(current); return true;
            }
        });
        // Let the remote desktop detect double-clicks; the default listener consumes the second tap.
        gestures.setOnDoubleTapListener(null);
        scales = new ScaleGestureDetector(context, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScaleBegin(ScaleGestureDetector detector) {
                if (mouseInput.hasVirtualButton()) return false;
                // Pinches can start anywhere on the touch surface, including letterboxing.
                if (viewport.width() <= 0 || viewport.height() <= 0) return false;
                viewportGesture = true;
                viewportPanActive = true;
                return true;
            }
            @Override public boolean onScale(ScaleGestureDetector detector) {
                viewport.zoomBy(detector.getScaleFactor(), detector.getFocusX(), detector.getFocusY());
                applyViewport();
                return true;
            }
            @Override public void onScaleEnd(ScaleGestureDetector detector) {
                viewport.finishGesture();
                applyViewport();
            }
        });
        scales.setQuickScaleEnabled(false);
        scales.setStylusScaleEnabled(false);
        cursor.setOnGenericMotionListener((v, event) -> isHardwareMouse(event)
            ? hardwareMouse(event) : point(event));
        setContentDescription(L10n.tr("远程桌面：轻触点击，拖动移动，长按右键，双指捏合缩放，放大后双指拖动画面，原始比例下双指上下滑动滚动"));
    }
    void videoSize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        frameWidth = width; frameHeight = height; fit();
    }
    void resetViewport() { releaseMouse(); viewport.reset(); applyViewport(); }
    static Rect previewCrop(int width,int height){
        int w=width,h=height;
        if((long)width*RemotePreviewStore.HEIGHT>(long)height*RemotePreviewStore.WIDTH)w=Math.max(1,height*RemotePreviewStore.WIDTH/RemotePreviewStore.HEIGHT);
        else h=Math.max(1,width*RemotePreviewStore.HEIGHT/RemotePreviewStore.WIDTH);
        return new Rect((width-w)/2,(height-h)/2,(width+w)/2,(height+h)/2);
    }
    void capturePreview(java.util.function.Consumer<Bitmap> completion){
        // Copy only the decoded-video Surface, excluding cursor, controls, keyboard and black margins.
        if(!isAttachedToWindow()||!video.getHolder().getSurface().isValid()){completion.accept(null);return;}
        Bitmap image=Bitmap.createBitmap(RemotePreviewStore.WIDTH,RemotePreviewStore.HEIGHT,Bitmap.Config.ARGB_8888);
        try{
            PixelCopy.request(video.getHolder().getSurface(),previewCrop(frameWidth,frameHeight),image,result->{
                if(result==PixelCopy.SUCCESS)completion.accept(image);else{image.recycle();completion.accept(null);}
            },new Handler(Looper.getMainLooper()));
        }catch(IllegalArgumentException error){image.recycle();completion.accept(null);}
    }
    private void fit() {
        if (getWidth() == 0 || getHeight() == 0) return;
        RemoteViewport active=pictureInPicture?pictureInPictureViewport:viewport;
        active.fit(getWidth(), getHeight(), frameWidth, frameHeight);
        android.view.ViewGroup.LayoutParams current=video.getLayoutParams();
        if(current.width!=active.baseWidth() || current.height!=active.baseHeight())
            video.setLayoutParams(new LayoutParams(active.baseWidth(),active.baseHeight(),android.view.Gravity.CENTER));
        applyViewport();
    }
    private void applyViewport() {
        // Transform the existing Surface without enlarging its decoder buffers.
        RemoteViewport active=pictureInPicture?pictureInPictureViewport:viewport;
        video.setScaleX(active.scale()); video.setScaleY(active.scale());
        video.setTranslationX(active.offsetX()); video.setTranslationY(active.offsetY());
        mouseInput.viewportWidth = active.width(); mouseInput.viewportHeight = active.height();
        cursor.invalidate();
    }
    private void sendMouse(float x, float y, int flag, int wheel) {
        this.x = x; this.y = y;
        if((flag==1||flag==3||flag==5)&&remoteFocusChanged!=null)remoteFocusChanged.run();
        session.pointer(x,y,flag,wheel);
        cursor.postInvalidateOnAnimation();
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh) { post(this::fit); }
    private void moveRelative(float dx, float dy) {
        if (pictureInPicture || viewport.width() <= 0 || viewport.height() <= 0) return;
        float previous=viewport.offsetY();
        viewport.panForPointer(y, dy);
        if(previous!=viewport.offsetY()) applyViewport();
        mouseInput.move(x + dx / viewport.width(), y + dy / viewport.height());
    }
    private boolean point(MotionEvent e) {
        boolean touchpad = relative && e.getToolType(0) != MotionEvent.TOOL_TYPE_MOUSE;
        float px=e.getX(),py=e.getY();
        if (!touchpad && !viewport.contains(px,py)) return false;
        if(touchpad){
            if(e.getActionMasked()==MotionEvent.ACTION_MOVE){
                moveRelative(px-lastX,py-lastY);
                lastX=px;lastY=py;
                return true;
            }
        }else{
            x=viewport.normalizedX(px);
            y=viewport.normalizedY(py);
        }
        lastX=px;lastY=py;
        mouseInput.move(x,y);
        return true;
    }
    private void click(int down, int up) { mouseInput.click(down,up); }
    private boolean handleTabletTouch(MotionEvent e) {
        int action = e.getActionMasked();
        int count = Math.min(e.getPointerCount(), 10);
        if (count <= 0 || viewport.width() <= 0 || viewport.height() <= 0) return false;
        if (action == MotionEvent.ACTION_DOWN) {
            if (!viewport.contains(e.getX(), e.getY())) return false;
            if (hasPointerCapture()) releasePointerCaptureSafe();
            if (remoteFocusChanged != null) remoteFocusChanged.run();
            tabletTouchActive = true;
        } else if (!tabletTouchActive) {
            return false;
        }
        int actionIndex = e.getActionIndex();
        int[] ids = new int[count];
        int[] phases = new int[count];
        float[] xs = new float[count];
        float[] ys = new float[count];
        for (int i = 0; i < count; i++) {
            ids[i] = e.getPointerId(i);
            xs[i] = Math.max(0f, Math.min(1f, viewport.normalizedX(e.getX(i))));
            ys[i] = Math.max(0f, Math.min(1f, viewport.normalizedY(e.getY(i))));
            if (action == MotionEvent.ACTION_CANCEL) {
                phases[i] = 3;
            } else if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) && i == actionIndex) {
                phases[i] = 0;
            } else if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && i == actionIndex) {
                phases[i] = 2;
            } else {
                phases[i] = 1;
            }
        }
        session.touch(ids, phases, xs, ys, count);
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            tabletTouchActive = false;
        }
        return true;
    }
    private boolean touch(MotionEvent e) {
        if (isHardwareMouse(e)) return hardwareMouse(e);
        if (tabletTouch && !relative && !pictureInPicture && !mouseInput.hasVirtualButton()) {
            return handleTabletTouch(e);
        }
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            directTap=!relative && viewport.contains(e.getX(),e.getY());
            tapConsumed=false;
            twoFinger = viewportGesture = viewportPanActive = false;
            touchRevision = mouseInput.revision();
            suppressPointerGesture = mouseInput.hasVirtualButton();
            if (hasPointerCapture()) releasePointerCaptureSafe();
        }
        if (action == MotionEvent.ACTION_POINTER_DOWN) {
            if (dragging) { mouseInput.gestureLeft(false); dragging = false; }
            twoFinger = true;
            twoFingerX = focus(e,true,-1); twoFingerY = scrollY = focus(e,false,-1);
            viewportPanActive = viewport.isZoomed() && viewport.contains(twoFingerX,twoFingerY);
            MotionEvent cancel = MotionEvent.obtain(e); cancel.setAction(MotionEvent.ACTION_CANCEL);
            gestures.onTouchEvent(cancel); cancel.recycle();
        }
        scales.onTouchEvent(e);
        if (twoFinger && action == MotionEvent.ACTION_MOVE && e.getPointerCount() >= 2) {
            if (mouseInput.hasVirtualButton()) return true;
            float focusX = focus(e,true,-1), focusY = focus(e,false,-1);
            if (viewport.isZoomed() || viewportGesture || scales.isInProgress()) {
                viewportGesture = true;
                if (!viewportPanActive) viewportPanActive = viewport.contains(focusX,focusY);
                if (viewportPanActive) {
                    viewport.panBy(focusX-twoFingerX,focusY-twoFingerY);
                    applyViewport();
                }
            } else if (viewport.contains(focusX,focusY)) {
                float delta = focusY - scrollY;
                if (Math.abs(delta) > 16 * getResources().getDisplayMetrics().density) {
                    session.pointer(x,y,7,delta > 0 ? 1 : -1); scrollY = focusY;
                }
            }
            twoFingerX = focusX; twoFingerY = focusY;
        } else if (!twoFinger) {
            if ((e.getButtonState() & MotionEvent.BUTTON_SECONDARY) != 0 && action == MotionEvent.ACTION_DOWN) {
                if (point(e)) click(3,4); return true;
            }
            gestures.onTouchEvent(e);
            if(action==MotionEvent.ACTION_UP && directTap && !tapConsumed && !dragging &&
               !suppressPointerGesture() && point(e)) cursor.performClick();
        }
        if (action == MotionEvent.ACTION_POINTER_UP) {
            // Rebase when a finger lifts; never resume a click/drag until all lift.
            twoFingerX = focus(e,true,e.getActionIndex());
            twoFingerY = scrollY = focus(e,false,e.getActionIndex());
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            if (dragging) mouseInput.gestureLeft(false);
            dragging = false;
            viewport.finishGesture(); applyViewport();
            viewportPanActive = false;
        }
        return true;
    }
    private static float focus(MotionEvent event, boolean horizontal, int excludedIndex) {
        float total = 0; int count = 0;
        for (int i=0;i<event.getPointerCount();i++) {
            if (i==excludedIndex) continue;
            total += horizontal ? event.getX(i) : event.getY(i); count++;
        }
        return total / Math.max(1,count);
    }
    @Override public boolean performClick() { super.performClick(); return true; }
    void targetFrameRate(int fps) {
        targetFrameRate = fps > 0 ? fps : 90;
        applySurfaceFrameRate(video.getHolder().getSurface());
    }
    private void applySurfaceFrameRate(android.view.Surface surface) {
        if (surface == null || !surface.isValid()) return;
        try {
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                surface.setFrameRate((float) targetFrameRate,
                    android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                    android.view.Surface.CHANGE_FRAME_RATE_ALWAYS);
            } else if (android.os.Build.VERSION.SDK_INT >= 30) {
                surface.setFrameRate((float) targetFrameRate,
                    android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            }
        } catch (Throwable ignored) { }
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { applySurfaceFrameRate(holder.getSurface()); session.surface(holder.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) { applySurfaceFrameRate(holder.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { releaseMouse(); session.surface(null); }
    private static boolean isHardwareMouse(MotionEvent e) {
        if (e.getToolType(0)==MotionEvent.TOOL_TYPE_FINGER &&
            e.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)) return false;
        return e.isFromSource(android.view.InputDevice.SOURCE_MOUSE)
            || e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
    }
    private void updateHardwareButtons(int buttons) {
        int changed = buttons ^ hardwareButtons;
        hardwareButtons = buttons;
        for (int button = 0; button < 3; button++) {
            int mask = 1 << button;
            if ((changed & mask) == 0) continue;
            boolean down = (buttons & mask) != 0;
            if (down && remoteFocusChanged != null) remoteFocusChanged.run();
            session.pointer(x, y, button * 2 + (down ? 1 : 2), 0);
        }
    }
    private boolean hardwareMouse(MotionEvent e) {
        if (pictureInPicture) return false;
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            implicitPrimary=false; updateHardwareButtons(0);
            return true;
        }
        if (action == MotionEvent.ACTION_HOVER_EXIT) return true;
        if (viewport.width() <= 0 || viewport.height() <= 0) return true;
        boolean inside = viewport.contains(e.getX(), e.getY());
        if (!inside && hardwareButtons == 0) return true;
        float nextX = viewport.normalizedX(e.getX()), nextY = viewport.normalizedY(e.getY());
        // Button and wheel messages already carry the current position.
        if (nextX != x || nextY != y) mouseInput.move(nextX, nextY);
        updateMouseEventButtons(e);
        if (action == MotionEvent.ACTION_HOVER_MOVE && captureMouse && !hasPointerCapture()) {
            requestFocus();
            requestPointerCapture();
        }
        if (action == MotionEvent.ACTION_SCROLL) sendWheel(e);
        return true;
    }
    private void updateMouseEventButtons(MotionEvent e) {
        int action=e.getActionMasked(), buttons=e.getButtonState();
        if(action==MotionEvent.ACTION_DOWN && buttons==0)implicitPrimary=true;
        if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_CANCEL)implicitPrimary=false;
        updateHardwareButtons(action==MotionEvent.ACTION_CANCEL ? 0 :
            buttons | (implicitPrimary ? MotionEvent.BUTTON_PRIMARY : 0));
    }
    private void sendWheel(MotionEvent e) {
        float amount = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
        if (amount != 0) session.pointer(x, y, 7, amount > 0 ? 1 : -1);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        return isHardwareMouse(e) ? hardwareMouse(e) : super.onGenericMotionEvent(e);
    }
    @Override public void onPointerCaptureChange(boolean captured) {
        super.onPointerCaptureChange(captured);
        if (!captured) {implicitPrimary=false;updateHardwareButtons(0);}
    }
    @Override public boolean onCapturedPointerEvent(MotionEvent e) {
        if (pictureInPicture || !captureMouse || viewport.width() <= 0 || viewport.height() <= 0) return true;
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_HOVER_MOVE) {
            float dx = e.getX(), dy = e.getY();
            for (int i = 0; i < e.getHistorySize(); i++) {
                dx += e.getHistoricalX(i);
                dy += e.getHistoricalY(i);
            }
            mouseInput.move(x + dx / viewport.width(), y + dy / viewport.height());
        }
        updateMouseEventButtons(e);
        if (action == MotionEvent.ACTION_SCROLL) sendWheel(e);
        return true;
    }
    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (android.os.Build.VERSION.SDK_INT >= 30)
            requestUnbufferedDispatch(android.view.InputDevice.SOURCE_CLASS_POINTER);
    }
    @Override protected void onDetachedFromWindow() { releasePointerCaptureSafe(); releaseMouse(); super.onDetachedFromWindow(); }
    private final class Cursor extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path arrow = new Path();
        private final Rect cursorDst = new Rect();
        private final float density;
        private final float arrowWidth = 12, arrowHeight = 15;
        Cursor(Context context) {
            super(context); setClickable(true); setContentDescription(L10n.tr("远程桌面触控区域"));
            density = context.getResources().getDisplayMetrics().density;
            // Match the iOS RemoteCursorArrowShape in density-independent units.
            arrow.moveTo(arrowWidth*.08f,arrowHeight*.04f);
            arrow.lineTo(arrowWidth*.08f,arrowHeight*.78f);
            arrow.lineTo(arrowWidth*.34f,arrowHeight*.60f);
            arrow.lineTo(arrowWidth*.55f,arrowHeight*.94f);
            arrow.lineTo(arrowWidth*.73f,arrowHeight*.84f);
            arrow.lineTo(arrowWidth*.52f,arrowHeight*.51f);
            arrow.lineTo(arrowWidth*.88f,arrowHeight*.49f);
            arrow.close();
        }
        @Override public android.view.PointerIcon onResolvePointerIcon(MotionEvent event,int index) {
            if (viewport.contains(event.getX(index),event.getY(index)))
                return android.view.PointerIcon.getSystemIcon(getContext(),android.view.PointerIcon.TYPE_NULL);
            return super.onResolvePointerIcon(event,index);
        }
        // GestureDetector calls performClick only after resolving tap vs drag/long press.
        @android.annotation.SuppressLint("ClickableViewAccessibility")
        @Override public boolean onTouchEvent(MotionEvent event) { return touch(event); }
        @Override public boolean performClick() { super.performClick(); click(1, 2); return true; }
        @Override public boolean onHoverEvent(MotionEvent e) {
            return isHardwareMouse(e) ? hardwareMouse(e) : super.onHoverEvent(e);
        }
        @Override protected void onDraw(Canvas canvas) {
            if (!host.visible) return;
            int saved=canvas.save();
            canvas.clipRect(viewport.left(),viewport.top(),viewport.left()+viewport.width(),viewport.top()+viewport.height());
            canvas.translate(viewport.left()+x*viewport.width(),viewport.top()+y*viewport.height());
            if(host.bitmap!=null){
                float scale=(viewport.width()/host.desktopWidth)*0.68f;
                float minBoxPx=17f*density;
                float maxBoxPx=25.5f*density;
                float bitmapMax=Math.max(host.bitmap.getWidth(),host.bitmap.getHeight());
                if(bitmapMax>0f){
                    float drawn=bitmapMax*scale;
                    if(drawn<minBoxPx) scale=minBoxPx/bitmapMax;
                    else if(drawn>maxBoxPx) scale=maxBoxPx/bitmapMax;
                }
                canvas.scale(scale,scale);
                paint.setColor(Color.WHITE);paint.setStyle(Paint.Style.FILL);paint.setFilterBitmap(true);
                cursorDst.set(-host.hotX,-host.hotY,host.bitmap.getWidth()-host.hotX,host.bitmap.getHeight()-host.hotY);
                canvas.drawBitmap(host.bitmap,null,cursorDst,paint);
                canvas.restoreToCount(saved); return;
            }
            canvas.scale(density*0.76f,density*0.76f);
            if(host.shape==7){
                paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.6f);paint.setColor(Color.BLACK);
                canvas.drawLine(0,-8,0,8,paint);canvas.drawLine(-3,-8,3,-8,paint);canvas.drawLine(-3,8,3,8,paint);
                canvas.restoreToCount(saved);return;
            }
            // Anchor the arrow tip to the same normalized coordinate sent to the desktop.
            canvas.translate(-arrowWidth*.08f,-arrowHeight*.04f);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.75f);paint.setColor(Color.BLACK);canvas.drawPath(arrow,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);canvas.drawPath(arrow,paint);
            canvas.restoreToCount(saved);
        }
    }
}
