package com.carma2.recomp;

import android.os.Build;
import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

public class Carma2Activity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Draw into the display cutout (the camera punch hole): without this Android keeps the app out of
        // that whole edge of the screen in landscape and the game does not reach the left side.
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        super.onCreate(savedInstanceState);
    }
}
