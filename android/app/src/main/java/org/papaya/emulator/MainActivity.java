package org.papaya.emulator;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Toast;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final int REQUEST_PICK_ROM = 1001;
    private SurfaceView surfaceView;
    private boolean isRunning = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);

        // Initialize Papaya native runtime with Potato Mode enabled
        PapayaNativeBridge.initRuntime(1, 2); // Potato Mode, Mobile High Tier (Snapdragon 8)

        // Open Storage Access Framework (SAF) picker to select ISO / ROM
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_PICK_ROM && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                try {
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                    if (pfd != null) {
                        int fd = pfd.detachFd();
                        long size = getContentResolver().openFileDescriptor(uri, "r").getStatSize();
                        PapayaNativeBridge.loadRomFd(fd, size);
                        isRunning = true;
                        Toast.makeText(this, "Booting ROM via Papaya Steam Engine...", Toast.LENGTH_LONG).show();
                    }
                } catch (Exception e) {
                    Toast.makeText(this, "Failed to open ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
                }
            }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (isRunning) {
            int action = event.getActionMasked();
            int buttons = (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE) ? 0x1000 : 0;
            int x = (int) ((event.getX() / surfaceView.getWidth() - 0.5f) * 65535.0f);
            int y = (int) ((event.getY() / surfaceView.getHeight() - 0.5f) * 65535.0f);
            PapayaNativeBridge.sendTouchInput(0, buttons, x, y);
        }
        return true;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        isRunning = false;
        PapayaNativeBridge.shutdown();
    }
}
