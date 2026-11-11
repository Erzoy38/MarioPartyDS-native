package io.github.erzoy38.mariopartyds;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

/** The game's activity: SDL's, with the two native libraries native/CMakeLists.txt builds. */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    /**
     * The game's native state lasts one run (the DS machine starts once per process), so when the
     * activity is really finished the process ends too, and the next start is a fresh one.
     */
    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            System.exit(0);
        }
    }

    /**
     * Called from native code (platform.cpp, "back to the main menu"): RestartActivity, in a process
     * of its own, ends this one and opens the app again at its start screen.
     */
    public void restartApp() {
        Intent intent = new Intent(this, RestartActivity.class);
        intent.putExtra(RestartActivity.EXTRA_PID, android.os.Process.myPid());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
    }
}
