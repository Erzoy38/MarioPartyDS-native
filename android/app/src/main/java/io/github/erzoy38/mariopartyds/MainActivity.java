package io.github.erzoy38.mariopartyds;

import org.libsdl.app.SDLActivity;

/** The game's activity: SDL's, with the two native libraries native/CMakeLists.txt builds. */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
