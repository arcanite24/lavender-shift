package studio.deimos.lavendershift;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.libsdl.app.SDLActivity;

/**
 * SDL's activity with the game's folders and arguments (docs/ANDROID.md),
 * after Pinyon Shift's.
 *
 * The files extracted from the player's disc and title update live in
 * game/disc and game/update, and the saves, settings, caches and logs in
 * user, under the first of the app's external files folders (internal
 * storage, then an SD card) that holds game/disc/default.xex. Those folders
 * need no permission and are reachable over adb and MTP.
 *
 * Tooling can set any environment variable with a string extra named
 * env.NAME and add game arguments with a string-array extra named args:
 *
 *   adb shell am start -n studio.deimos.lavendershift/.LavenderShiftActivity \
 *       --esa args --user_data_root=/sdcard/.../test-user
 *
 * A later argument wins, so an args extra overrides the folders below.
 */
public class LavenderShiftActivity extends SDLActivity {
    private static final String TAG = "LavenderShift";
    /** Extra arguments, one per line, read at every start. */
    static final String ARGUMENTS_FILE = "user/config/android-arguments.txt";
    private File base;
    private File driversDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        base = gameBase();
        setDefaultEnvironment("HOME", getFilesDir().getAbsolutePath());
        // Custom GPU drivers (Mesa Turnip), chosen with android_gpu_driver;
        // the runtime copies the chosen one inside the app before loading it.
        driversDir = new File(base, "user/drivers");
        setDefaultEnvironment("REX_ANDROID_DRIVERS_DIR", driversDir.getAbsolutePath());
        // Drivers packaged with the game (the ones Pinyon Shift ships), and
        // the one recommended for this GPU, which android_gpu_driver "auto"
        // (the default) loads.
        installBundledDrivers();
        String adreno = adrenoModel();
        if (adreno != null) {
            setDefaultEnvironment("LAVENDER_SHIFT_ADRENO_MODEL", adreno);
            String recommended = recommendedDriver(adreno);
            if (recommended != null) {
                setDefaultEnvironment("REX_ANDROID_RECOMMENDED_DRIVER", recommended);
            }
        }
        // The build's provenance, packaged as an asset, for logs.
        File manifest = new File(getFilesDir(), "lavender_shift_build.json");
        try (InputStream in = getAssets().open("lavender_shift_build.json");
             OutputStream out = new FileOutputStream(manifest)) {
            copy(in, out);
            setEnvironment("LAVENDER_SHIFT_BUILD_MANIFEST", manifest.getAbsolutePath());
        } catch (IOException error) {
            Log.w(TAG, "no build manifest in the package", error);
        }
        Intent intent = getIntent();
        Bundle extras = intent != null ? intent.getExtras() : null;
        if (extras != null) {
            for (String key : extras.keySet()) {
                if (key.startsWith("env.")) {
                    Object value = extras.get(key);
                    if (value != null) {
                        setEnvironment(key.substring(4), value.toString());
                    }
                }
            }
        }
        super.onCreate(savedInstanceState);
        // A race played on a controller touches nothing, and a screen that
        // times out sends the game to the background. Only while this window
        // shows.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (!new File(base, "game/disc/default.xex").isFile()) {
            Toast.makeText(this, "Lavender Shift: copy the game files to "
                    + new File(base, "game").getAbsolutePath() + " first", Toast.LENGTH_LONG).show();
        }
    }

    // The first external files folder holding the game, else the first one.
    private File gameBase() {
        File[] folders = getExternalFilesDirs(null);
        File first = null;
        if (folders != null) {
            for (File folder : folders) {
                if (folder == null) continue;
                if (first == null) first = folder;
                if (new File(folder, "game/disc/default.xex").isFile()) return folder;
            }
        }
        return first != null ? first : getFilesDir();
    }

    // The C++ runtime first, then the ReXGlue runtime, which holds SDL and
    // registers its Java natives when loaded, then the game (libmain.so),
    // whose SDL_main SDL calls.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    // The native libraries are built for ARMv8.2-A: an older core would crash
    // on the first LSE atomic. SDL shows a link error from here as a dialog.
    @Override
    public void loadLibraries() {
        if (!cpuHasFeatures("atomics", "asimdrdm")) {
            throw new UnsatisfiedLinkError("Lavender Shift needs an ARMv8.2 CPU "
                    + "(Cortex-A55, Cortex-A75 or newer); this device's CPU is older.");
        }
        super.loadLibraries();
    }

    private static boolean cpuHasFeatures(String... features) {
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (String line; (line = reader.readLine()) != null; ) {
                if (!line.startsWith("Features")) continue;
                List<String> listed = java.util.Arrays.asList(
                        line.substring(line.indexOf(':') + 1).trim().split("\\s+"));
                for (String feature : features) {
                    if (!listed.contains(feature)) return false;
                }
                return true;
            }
        } catch (IOException ignored) {
            // Unreadable: let the libraries load.
        }
        return true;
    }

    // The folders and the flags tools/launch.ps1 passes on Windows, then the
    // lines of user/config/android-arguments.txt, then the intent's args.
    @Override
    protected String[] getArguments() {
        List<String> arguments = new ArrayList<>();
        arguments.add("--game_data_root=" + new File(base, "game/disc").getAbsolutePath());
        arguments.add("--update_data_root=" + new File(base, "game/update").getAbsolutePath());
        arguments.add("--user_data_root=" + new File(base, "user").getAbsolutePath());
        // On Windows the log sits beside the executable; the package's
        // folder is read-only here.
        arguments.add("--log_file=" + new File(base, "user/logs/runtime.log").getAbsolutePath());
        arguments.add("--xex_apply_patches=true");
        arguments.add("--gpu_backend=vulkan");
        arguments.add("--xam_xstudio_request_unsupported=true");
        arguments.add("--gpu_read_pointer_publish_dwords=1024");
        File file = new File(base, ARGUMENTS_FILE);
        if (file.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
                for (String line; (line = reader.readLine()) != null; ) {
                    if (line.trim().startsWith("--")) arguments.add(line.trim());
                }
            } catch (IOException error) {
                Log.w(TAG, "launch arguments not read", error);
            }
        }
        Intent intent = getIntent();
        String[] given = intent != null ? intent.getStringArrayExtra("args") : null;
        if (given != null) Collections.addAll(arguments, given);
        return lastOfEach(arguments).toArray(new String[0]);
    }

    // The game's parser rejects an option given twice and then applies none
    // of them, so a later --name=value replaces an earlier one here.
    static List<String> lastOfEach(List<String> arguments) {
        java.util.LinkedHashMap<String, String> byName = new java.util.LinkedHashMap<>();
        List<String> others = new ArrayList<>();
        for (String argument : arguments) {
            int equals = argument.indexOf('=');
            if (!argument.startsWith("--") || equals < 0) {
                others.add(argument);
                continue;
            }
            String name = argument.substring(0, equals);
            byName.remove(name);
            byName.put(name, argument);
        }
        List<String> result = new ArrayList<>(byName.values());
        result.addAll(others);
        return result;
    }

    // The Adreno GPU's model number ("740"), from the kernel driver, or from
    // the SoC where that is not readable; null when not an Adreno.
    private static String adrenoModel() {
        try (BufferedReader reader = new BufferedReader(
                new FileReader("/sys/class/kgsl/kgsl-3d0/gpu_model"))) {
            String line = reader.readLine();
            Matcher match = line != null ? Pattern.compile("Adreno(\\d{3})").matcher(line) : null;
            if (match != null && match.find()) {
                return match.group(1);
            }
        } catch (IOException ignored) {
            // Not readable here: the SoC below.
        }
        if (Build.VERSION.SDK_INT >= 31) {
            switch (Build.SOC_MODEL) {
                case "SM8450": case "SM8475": return "730";
                case "SM8550": case "QCS8550": return "740";
                case "SM8635": return "735";
                case "SM8650": return "750";
                case "SM7475": return "725";
                case "SM7550": return "720";
                case "SM7675": return "732";
                case "SM8750": return "830";
                default: break;
            }
        }
        return null;
    }

    // Adreno 7xx and 8xx: Mesa Turnip Gen8 V37, which Pinyon Shift measured
    // fastest there. Others keep the system's.
    private static String recommendedDriver(String adreno) {
        return adreno.startsWith("7") || adreno.startsWith("8") ? "turnip-gen8-v37" : null;
    }

    // Copies each driver folder under the package's assets/drivers into
    // user/drivers, where a file is missing or differs in size.
    private void installBundledDrivers() {
        try {
            String[] names = getAssets().list("drivers");
            if (names == null) return;
            for (String name : names) {
                String[] files = getAssets().list("drivers/" + name);
                if (files == null || files.length == 0) continue;
                File folder = new File(driversDir, name);
                folder.mkdirs();
                for (String file : files) {
                    File target = new File(folder, file);
                    long size;
                    try (InputStream in = getAssets().open("drivers/" + name + "/" + file)) {
                        size = 0;
                        byte[] buffer = new byte[65536];
                        for (int read; (read = in.read(buffer)) > 0; ) size += read;
                    }
                    if (target.length() == size) continue;
                    try (InputStream in = getAssets().open("drivers/" + name + "/" + file);
                         OutputStream out = new FileOutputStream(target)) {
                        copy(in, out);
                    }
                    Log.i(TAG, "bundled GPU driver " + name + ": " + file + " installed");
                }
            }
        } catch (IOException error) {
            Log.w(TAG, "bundled GPU drivers not installed", error);
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[65536];
        for (int read; (read = in.read(buffer)) > 0; ) {
            out.write(buffer, 0, read);
        }
    }

    private static void setDefaultEnvironment(String name, String value) {
        if (Os.getenv(name) == null) {
            setEnvironment(name, value);
        }
    }

    private static void setEnvironment(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException error) {
            Log.e(TAG, "setenv " + name + " failed", error);
        }
    }
}
