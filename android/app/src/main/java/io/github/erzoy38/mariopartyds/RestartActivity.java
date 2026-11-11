package io.github.erzoy38.mariopartyds;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Starts the app again in a new process: runs in its own process (":restart" in the manifest), ends
 * the game's process, opens MainActivity and ends itself. Nothing of it is seen.
 */
public class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) {
            android.os.Process.killProcess(pid);
        }
        Intent start = new Intent(this, MainActivity.class);
        start.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(start);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
