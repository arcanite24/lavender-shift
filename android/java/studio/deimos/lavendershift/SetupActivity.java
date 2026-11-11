package studio.deimos.lavendershift;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The app's entry point, after Pinyon Shift's. With the game files in place
 * it starts the game at once. Without them, or when opened from the "Get
 * files from PC" shortcut, it gets them without adb or developer options:
 * from the PC launcher's Android panel over the local network, found by a
 * UDP broadcast and paired with the six-digit code the panel shows, or from
 * a folder copied to the device over USB, through the system's folder
 * picker.
 *
 * Files go where LavenderShiftActivity plays them from, under one of the
 * app's external files folders (internal storage or an SD card, chosen here
 * before the first copy): the game in game/disc and game/update, the saves,
 * settings and caches in user. A copy that stops resumes where it was. The
 * save and the settings are replaced only when chosen here, after a backup
 * into user/backups.
 *
 * The launcher shares on TCP 47625 and answers discovery on UDP 47626
 * (Pinyon Shift's launcher uses 47615 and 47616), so each app finds only
 * its own launcher.
 *
 * When the share offers a newer build of this app than the one installed,
 * this screen says so and hands it to the system installer.
 */
public class SetupActivity extends Activity implements View.OnClickListener {
    private static final String TAG = "LavenderShiftSetup";
    private static final int HTTP_PORT = 47625;
    private static final int DISCOVERY_PORT = 47626;
    private static final String DISCOVER = "LAVENDER-SHIFT-DISCOVER 1";
    private static final String HERE = "LAVENDER-SHIFT-HERE";
    private static final String API = "/lavender/v1/";
    private static final String CODE_HEADER = "X-Lavender-Code";
    private static final int REQUEST_FOLDER = 0x4C53;
    private static final int BACKGROUND = Color.rgb(14, 11, 18);
    private static final int ACCENT = Color.rgb(181, 154, 240);
    private static final int MUTED = Color.rgb(165, 154, 179);
    static final String GAME_FILE = "game/disc/default.xex";
    // Game files being copied: the game does not start from a partial copy.
    static final String INCOMPLETE = "game/.copy-incomplete";
    // The game's settings file, replaced by the PC's only when chosen.
    private static final String SETTINGS_FILE = "user/config/lavender_shift.toml";
    // The PC's save being copied, after the device's moved to a backup.
    private static final String SAVE_INCOMPLETE = "user/backups/.pc-save-incomplete";
    // Profiles shared by every player (DLC packages): not part of the save.
    private static final String SHARED_PROFILE = "0000000000000000";
    // Experimental, opt-in: accept GPUs without vertexPipelineStoresAndAtomics
    // (Mali), skipping vertex shader memory export.
    private static final String[] VERTEX_STORES_FALLBACK = {
            "--vulkan_require_vertex_pipeline_stores_and_atomics=false"};
    // The app shortcut "Get files from PC" opens this screen with the game installed.
    private static final String SETUP_ACTION = "studio.deimos.lavendershift.SETUP";
    // The system installer's answer for an app update.
    private static final String INSTALL_STATUS = "studio.deimos.lavendershift.INSTALL_STATUS";

    private File base;
    private final List<File> volumes = new ArrayList<>();
    private TextView intro;
    private TextView storageTitle;
    private RadioGroup storageGroup;
    private TextView discoveryText;
    private EditText addressField;
    private EditText codeField;
    private Button connectButton;
    private LinearLayout groupsBox;
    private TextView spaceText;
    private Button copyButton;
    private ProgressBar progress;
    private TextView progressText;
    private Button folderButton;
    private Button playButton;
    private TextView updateText;
    private Button updateButton;
    private CheckBox vertexStoresCheck;
    private BroadcastReceiver installReceiver;
    private final Map<String, CheckBox> groupChecks = new LinkedHashMap<>();
    private JSONObject manifest;
    private String server;
    private volatile boolean busy;
    private volatile boolean discovering;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        base = chooseBase();
        Intent intent = getIntent();
        boolean asked = intent != null && (SETUP_ACTION.equals(intent.getAction())
                || intent.getBooleanExtra("setup", false));
        if (gameReady() && !asked) {
            startGame();
            return;
        }
        buildLayout();
        refresh();
        startDiscovery();
        installReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent status) {
                installStatus(status);
            }
        };
        // Sent by the system through this app's own PendingIntent: not exported.
        registerReceiver(installReceiver, new IntentFilter(INSTALL_STATUS), Context.RECEIVER_NOT_EXPORTED);
    }

    @Override
    protected void onDestroy() {
        discovering = false;
        if (installReceiver != null) unregisterReceiver(installReceiver);
        super.onDestroy();
    }

    // The folder LavenderShiftActivity plays from: the first external files
    // folder with the game; else one with a copy that stopped, to resume it;
    // else the first (internal storage), which the storage choice can change
    // before the first copy.
    private File chooseBase() {
        volumes.clear();
        File[] folders = getExternalFilesDirs(null);
        if (folders != null) {
            for (File folder : folders) {
                if (folder != null) volumes.add(folder);
            }
        }
        for (File folder : volumes) {
            if (new File(folder, GAME_FILE).isFile()) return folder;
        }
        for (File folder : volumes) {
            if (new File(folder, INCOMPLETE).exists()) return folder;
        }
        return volumes.isEmpty() ? getFilesDir() : volumes.get(0);
    }

    // Whether the game files are on a volume already (complete or not): then
    // the storage is not chosen any more.
    private boolean gameStarted() {
        return new File(base, GAME_FILE).isFile() || new File(base, INCOMPLETE).exists();
    }

    private boolean gameReady() {
        return new File(base, GAME_FILE).isFile() && !new File(base, INCOMPLETE).exists();
    }

    private void startGame() {
        Intent game = new Intent(this, LavenderShiftActivity.class);
        Intent intent = getIntent();
        if (intent != null && intent.getExtras() != null) {
            game.putExtras(intent.getExtras());
            game.removeExtra("setup");
        }
        startActivity(game);
        finish();
    }

    // ---- Layout -------------------------------------------------------

    private int dp(float value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics()));
    }

    private TextView text(LinearLayout parent, String value, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        view.setTextColor(color);
        view.setLineSpacing(0, 1.15f);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        parent.addView(view, params);
        return view;
    }

    private Button button(LinearLayout parent, String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(this);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        parent.addView(button, params);
        return button;
    }

    private EditText field(LinearLayout parent, String hint, int type) {
        EditText field = new EditText(this);
        field.setHint(hint);
        field.setInputType(type);
        field.setSingleLine(true);
        field.setTextColor(Color.WHITE);
        field.setHintTextColor(MUTED);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                dp(320), LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(4);
        parent.addView(field, params);
        return field;
    }

    private void buildLayout() {
        getWindow().setStatusBarColor(BACKGROUND);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(28), dp(24), dp(28), dp(32));
        scroll.addView(column);

        text(column, "Lavender Shift", 26, Color.WHITE, true);
        intro = text(column, "", 15, MUTED, false);
        // No Qualcomm kernel GPU driver: probably Mali (Tensor, Exynos, Dimensity), which
        // the game refuses at start unless the opt-in at the bottom is ticked.
        if (!new File("/sys/class/kgsl/kgsl-3d0").exists() && !hasArguments(VERTEX_STORES_FALLBACK)) {
            text(column, "This device does not look like a Snapdragon (Adreno). If the game closes at "
                    + "start, scroll down to Experimental and tick \"Allow GPUs without vertex "
                    + "shader stores\", then try again.", 14, ACCENT, false)
                    .setPadding(0, dp(12), 0, 0);
        }

        // Where the game goes, before the first copy, when there is an SD card.
        storageTitle = text(column, "Storage", 18, ACCENT, true);
        storageTitle.setPadding(0, dp(16), 0, 0);
        storageGroup = new RadioGroup(this);
        for (int i = 0; i < volumes.size(); i++) {
            RadioButton choice = new RadioButton(this);
            choice.setId(View.generateViewId());
            choice.setTag(i);
            choice.setTextColor(Color.WHITE);
            choice.setText(volumeName(volumes.get(i), i) + " (" + size(volumes.get(i).getUsableSpace()) + " free)");
            storageGroup.addView(choice);
            if (volumes.get(i).equals(base)) choice.setChecked(true);
        }
        storageGroup.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int id) {
                View choice = group.findViewById(id);
                if (choice == null || busy || gameStarted()) return;
                base = volumes.get((Integer) choice.getTag());
                // The launch arguments live beside the game: follow it.
                if (vertexStoresCheck.isChecked()) {
                    try {
                        setArguments(VERTEX_STORES_FALLBACK, true);
                    } catch (IOException error) {
                        Log.w(TAG, "launch arguments not written", error);
                    }
                }
                if (manifest != null) updateSpace();
            }
        });
        column.addView(storageGroup);

        text(column, "From your PC over Wi-Fi", 18, ACCENT, true).setPadding(0, dp(16), 0, 0);
        text(column, "On the PC, open the Lavender Shift launcher, choose Android, then Share on Wi-Fi, "
                + "and keep that panel open. This device and the PC must be on the same network.",
                14, MUTED, false);
        discoveryText = text(column, "Looking for the PC…", 14, Color.WHITE, false);
        addressField = field(column, "PC address, such as 192.168.1.20",
                InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        codeField = field(column, "Six-digit code from the launcher", InputType.TYPE_CLASS_NUMBER);
        codeField.setFilters(new InputFilter[] {new InputFilter.LengthFilter(6)});
        connectButton = button(column, "Connect");
        updateText = text(column, "", 14, ACCENT, false);
        updateText.setVisibility(View.GONE);
        updateButton = button(column, "Install update");
        updateButton.setVisibility(View.GONE);
        groupsBox = new LinearLayout(this);
        groupsBox.setOrientation(LinearLayout.VERTICAL);
        column.addView(groupsBox);
        spaceText = text(column, "", 13, MUTED, false);
        spaceText.setVisibility(View.GONE);
        copyButton = button(column, "Copy to this device");
        copyButton.setVisibility(View.GONE);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        barParams.topMargin = dp(12);
        column.addView(progress, barParams);
        progressText = text(column, "", 13, Color.WHITE, false);

        text(column, "From a folder", 18, ACCENT, true).setPadding(0, dp(20), 0, 0);
        text(column, "Or copy the extracted game folder (the install folder's .local\\game on the PC, "
                + "holding disc and update) to this device over USB, for example into Download, and "
                + "choose it here. It is copied into the app; you can delete the copy afterwards.",
                14, MUTED, false);
        folderButton = button(column, "Choose folder…");

        text(column, "Experimental", 18, ACCENT, true).setPadding(0, dp(20), 0, 0);
        text(column, "For GPUs the game closes on at start, such as Mali (Google Tensor, Exynos, "
                + "Dimensity). Vertex shader memory writes are skipped, so some effects or results "
                + "may differ. Leave it off on Snapdragon.",
                14, MUTED, false);
        vertexStoresCheck = new CheckBox(this);
        vertexStoresCheck.setTextColor(Color.WHITE);
        vertexStoresCheck.setText("Allow GPUs without vertex shader stores");
        vertexStoresCheck.setChecked(hasArguments(VERTEX_STORES_FALLBACK));
        vertexStoresCheck.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton view, boolean checked) {
                try {
                    setArguments(VERTEX_STORES_FALLBACK, checked);
                } catch (IOException error) {
                    Log.w(TAG, "launch arguments not written", error);
                }
            }
        });
        column.addView(vertexStoresCheck);

        playButton = button(column, "Play");
        playButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        ((LinearLayout.LayoutParams) playButton.getLayoutParams()).topMargin = dp(24);
        setContentView(scroll);
    }

    private static String volumeName(File folder, int index) {
        boolean removable;
        try {
            removable = Environment.isExternalStorageRemovable(folder);
        } catch (IllegalArgumentException error) {
            removable = index > 0;
        }
        return removable ? "SD card" + (index > 1 ? " " + index : "") : "Internal storage";
    }

    @Override
    public void onClick(View view) {
        if (view == connectButton) connect();
        else if (view == copyButton) startCopy();
        else if (view == folderButton) chooseFolder();
        else if (view == playButton) startGame();
        else if (view == updateButton) installUpdate();
    }

    // UI changes from the worker threads.
    private void later(final TextView view, final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                view.setText(text);
            }
        });
    }

    private void refreshLater() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                refresh();
            }
        });
    }

    private void refresh() {
        boolean ready = gameReady();
        intro.setText(ready
                ? "The game is installed. Copy more from your PC here: DLC, the free-roam pipelines, "
                  + "your PC's save or settings, or a fresh copy of the game files."
                : "The game files are not on this device yet. They come from your PC, where the game was "
                  + "built from your own disc and title update.");
        // The storage is chosen once, before the game files are copied.
        boolean choose = volumes.size() > 1 && !gameStarted();
        storageTitle.setVisibility(choose ? View.VISIBLE : View.GONE);
        storageGroup.setVisibility(choose ? View.VISIBLE : View.GONE);
        for (int i = 0; i < storageGroup.getChildCount(); i++) storageGroup.getChildAt(i).setEnabled(!busy);
        playButton.setEnabled(ready && !busy);
        connectButton.setEnabled(!busy);
        folderButton.setEnabled(!busy);
        copyButton.setEnabled(!busy);
        updateButton.setEnabled(!busy);
    }

    // ---- Discovery and pairing ------------------------------------------

    private void startDiscovery() {
        discovering = true;
        new Thread(new Runnable() {
            @Override
            public void run() {
                discover();
            }
        }, "PcDiscovery").start();
    }

    private void discover() {
        byte[] request = DISCOVER.getBytes(StandardCharsets.US_ASCII);
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setBroadcast(true);
            socket.setSoTimeout(1500);
            long until = System.currentTimeMillis() + 60_000;
            while (discovering && System.currentTimeMillis() < until) {
                for (InetAddress target : broadcastAddresses()) {
                    try {
                        socket.send(new DatagramPacket(request, request.length, target, DISCOVERY_PORT));
                    } catch (IOException ignored) {
                        // An interface without broadcast; the others still try.
                    }
                }
                try {
                    byte[] buffer = new byte[256];
                    DatagramPacket reply = new DatagramPacket(buffer, buffer.length);
                    socket.receive(reply);
                    String[] parts = new String(buffer, 0, reply.getLength(), StandardCharsets.US_ASCII)
                            .trim().split(" ", 4);
                    if (parts.length >= 3 && parts[0].equals(HERE)) {
                        final String address = reply.getAddress().getHostAddress() + ":" + parts[2];
                        final String name = parts.length > 3 ? parts[3] : "the PC";
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                discoveryText.setText("Found " + name + " at " + address
                                        + ". Enter the code the launcher shows.");
                                if (addressField.getText().length() == 0) addressField.setText(address);
                                codeField.requestFocus();
                            }
                        });
                        return;
                    }
                } catch (SocketTimeoutException ignored) {
                    // Asked again below.
                }
            }
            later(discoveryText, "The PC was not found by itself. Enter the address the launcher shows.");
        } catch (IOException error) {
            Log.w(TAG, "discovery failed", error);
            later(discoveryText, "Enter the address the launcher shows.");
        }
    }

    private static List<InetAddress> broadcastAddresses() {
        List<InetAddress> result = new ArrayList<>();
        try {
            for (NetworkInterface adapter : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!adapter.isUp() || adapter.isLoopback()) continue;
                for (InterfaceAddress address : adapter.getInterfaceAddresses()) {
                    if (address.getBroadcast() != null) result.add(address.getBroadcast());
                }
            }
            result.add(InetAddress.getByName("255.255.255.255"));
        } catch (IOException ignored) {
            // No interfaces readable: nothing to send to.
        }
        return result;
    }

    private void connect() {
        String address = addressField.getText().toString().trim()
                .replaceFirst("^https?://", "").replaceAll("/.*$", "");
        final String code = codeField.getText().toString().trim();
        if (address.isEmpty() || code.length() != 6) {
            progressText.setText("Enter the PC's address and the six-digit code.");
            return;
        }
        final String host = address.contains(":") ? address : address + ":" + HTTP_PORT;
        busy = true;
        refresh();
        progressText.setText("Connecting to " + host + "…");
        new Thread(new Runnable() {
            @Override
            public void run() {
                fetchManifest(host, code);
            }
        }, "PcConnect").start();
    }

    private void fetchManifest(final String host, String code) {
        try {
            HttpURLConnection connection = open("http://" + host + API + "manifest", code, 0);
            int status = connection.getResponseCode();
            if (status == 403) throw new RefusedException(readError(connection));
            if (status != 200) throw new IOException("the PC answered " + status);
            final JSONObject result = new JSONObject(readAll(connection.getInputStream()));
            discovering = false;
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    server = host;
                    manifest = result;
                    showGroups();
                    showUpdate();
                    progressText.setText("Connected to " + result.optString("name", host) + ".");
                }
            });
        } catch (IOException | JSONException error) {
            later(progressText, "Could not connect: " + error.getMessage()
                    + ". Check that the launcher is sharing and both are on the same network.");
        } finally {
            busy = false;
            refreshLater();
        }
    }

    private HttpURLConnection open(String url, String code, long offset) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(8000);
        connection.setReadTimeout(30000);
        connection.setRequestProperty(CODE_HEADER, code);
        if (offset > 0) connection.setRequestProperty("Range", "bytes=" + offset + "-");
        return connection;
    }

    private static String readError(HttpURLConnection connection) {
        try (InputStream in = connection.getErrorStream()) {
            return in != null ? readAll(in).trim() : "refused";
        } catch (IOException error) {
            return "refused";
        }
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        copy(in, out, null);
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private void showGroups() {
        groupsBox.removeAllViews();
        groupChecks.clear();
        JSONArray groups = manifest.optJSONArray("groups");
        if (groups == null) return;
        boolean ready = gameReady();
        for (int i = 0; i < groups.length(); i++) {
            JSONObject group = groups.optJSONObject(i);
            if (group == null) continue;
            final String id = group.optString("id");
            CheckBox check = new CheckBox(this);
            check.setTextColor(Color.WHITE);
            String label = group.optString("label") + " (" + size(group.optLong("bytes")) + ")";
            if (id.equals("save")) {
                label += ": replaces this device's save; the current one is backed up first";
            } else if (id.equals("settings")) {
                label += ": replaces this device's settings, backed up first. Graphics options chosen "
                        + "for the PC may be slow or unavailable here";
            }
            check.setText(label);
            if (id.equals("game")) {
                // Needed to play; a complete copy is only refreshed on request.
                check.setChecked(!ready);
                check.setEnabled(ready);
            } else {
                check.setChecked(group.optBoolean("selected"));
            }
            check.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
                @Override
                public void onCheckedChanged(CompoundButton view, boolean checked) {
                    updateSpace();
                }
            });
            groupsBox.addView(check);
            groupChecks.put(id, check);
        }
        spaceText.setVisibility(View.VISIBLE);
        copyButton.setVisibility(View.VISIBLE);
        updateSpace();
    }

    private long selectedBytes() {
        long total = 0;
        JSONArray groups = manifest.optJSONArray("groups");
        for (int i = 0; groups != null && i < groups.length(); i++) {
            JSONObject group = groups.optJSONObject(i);
            CheckBox check = groupChecks.get(group.optString("id"));
            if (check != null && check.isChecked()) total += group.optLong("bytes");
        }
        return total;
    }

    private void updateSpace() {
        spaceText.setText("Selected: " + size(selectedBytes()) + ". Free on this device: "
                + size(base.getUsableSpace()) + ".");
    }

    private static String size(long bytes) {
        if (bytes >= 1L << 30) return String.format(Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30));
        if (bytes >= 1L << 20) return String.format(Locale.ROOT, "%.0f MB", bytes / (double) (1L << 20));
        return String.format(Locale.ROOT, "%.0f KB", Math.max(1, bytes / 1024.0));
    }

    // ---- Copy over the network --------------------------------------

    private void startCopy() {
        if (manifest == null || server == null) return;
        final String code = codeField.getText().toString().trim();
        final List<String> chosen = new ArrayList<>();
        for (Map.Entry<String, CheckBox> entry : groupChecks.entrySet()) {
            if (entry.getValue().isChecked()) chosen.add(entry.getKey());
        }
        if (chosen.isEmpty()) return;
        busy = true;
        refresh();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        progress.setVisibility(View.VISIBLE);
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    copyFromPc(code, chosen);
                    finish("Done. Press Play.", true);
                } catch (IOException | JSONException error) {
                    Log.e(TAG, "copy failed", error);
                    finish("The copy stopped: " + error.getMessage()
                            + ". Press Copy again to continue where it stopped.", false);
                }
            }
        }, "PcCopy").start();
    }

    // The end of a copy, on a worker thread.
    private void finish(final String message, final boolean success) {
        busy = false;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (success) progress.setProgress(progress.getMax());
                progressText.setText(message);
                refresh();
            }
        });
    }

    private void copyFromPc(String code, List<String> chosen) throws IOException, JSONException {
        List<JSONObject> files = new ArrayList<>();
        long total = 0;
        long needed = 0;
        JSONArray groups = manifest.getJSONArray("groups");
        boolean game = chosen.contains("game");
        for (int i = 0; i < groups.length(); i++) {
            JSONObject group = groups.getJSONObject(i);
            if (!chosen.contains(group.getString("id"))) continue;
            JSONArray list = group.getJSONArray("files");
            for (int j = 0; j < list.length(); j++) {
                JSONObject file = list.getJSONObject(j);
                File target = destination(file.getString("path"));
                long size = file.getLong("size");
                total += size;
                if (target.length() != size) needed += size;
                files.add(file);
            }
        }
        if (needed > base.getUsableSpace() - (256L << 20)) {
            throw new IOException("not enough free space: " + size(needed) + " needed, "
                    + size(base.getUsableSpace()) + " free");
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        // The device's save moves aside whole, so no file of it mixes with
        // the PC's; its files are then all fetched.
        if (chosen.contains("save")) backUpSave(stamp);
        if (chosen.contains("settings")) backUpSettings(stamp);
        File incomplete = new File(base, INCOMPLETE);
        if (game) {
            incomplete.getParentFile().mkdirs();
            new FileOutputStream(incomplete).close();
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    refresh();  // the storage is now fixed
                }
            });
        }
        long[] done = {0};
        long started = System.currentTimeMillis();
        for (int i = 0; i < files.size(); i++) {
            JSONObject file = files.get(i);
            long size = file.getLong("size");
            File target = destination(file.getString("path"));
            if (target.length() != size) {
                fetch(code, API + "file/" + file.getInt("index"), target, size, done, total, i + 1,
                        files.size(), started);
            } else {
                done[0] += size;
            }
        }
        if (game) incomplete.delete();
        if (chosen.contains("save")) new File(base, SAVE_INCOMPLETE).delete();
    }

    // ---- App update -----------------------------------------------------

    // The package the PC offers, when it is a newer build of this app.
    private void showUpdate() {
        updateText.setVisibility(View.GONE);
        updateButton.setVisibility(View.GONE);
        JSONObject apk = manifest.optJSONObject("apk");
        if (apk == null) return;
        long offeredCode = apk.optLong("version_code");
        long installedCode;
        String installedVersion;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(),
                    PackageManager.PackageInfoFlags.of(0));
            installedCode = info.getLongVersionCode();
            installedVersion = info.versionName;
        } catch (PackageManager.NameNotFoundException error) {
            return;
        }
        String installedBuilt = "";
        try (InputStream in = getAssets().open("lavender_shift_build.json")) {
            installedBuilt = new JSONObject(readAll(in)).optString("built_utc");
        } catch (IOException | JSONException error) {
            Log.w(TAG, "no build manifest in the package", error);
        }
        // A lower version code cannot install over this one. Packages of one
        // version compare by when they were packaged (ISO 8601 in UTC from
        // lavender_android.py), so a rebuild with only app changes counts too.
        String offeredBuilt = apk.optString("built_utc");
        boolean newer = offeredCode > installedCode
                || (offeredCode == installedCode && !offeredBuilt.isEmpty()
                    && offeredBuilt.compareTo(installedBuilt) > 0);
        if (!newer) return;
        String when = offeredBuilt.length() >= 19 ? offeredBuilt.substring(0, 19).replace('T', ' ') : offeredBuilt;
        updateText.setText("The PC has a newer build of Lavender Shift (" + apk.optString("version") + ", built "
                + when + " UTC; this device has " + installedVersion + "). Your save and settings stay as "
                + "they are.");
        updateText.setVisibility(View.VISIBLE);
        updateButton.setVisibility(View.VISIBLE);
    }

    private void installUpdate() {
        if (manifest == null || server == null) return;
        if (!getPackageManager().canRequestPackageInstalls()) {
            // Android asks once whether this app may install apps.
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception error) {
                Log.w(TAG, "no install permission screen", error);
            }
            progressText.setText("Allow Lavender Shift to install apps, come back, and press Install update again.");
            return;
        }
        final JSONObject apk = manifest.optJSONObject("apk");
        if (apk == null) return;
        final String code = codeField.getText().toString().trim();
        busy = true;
        refresh();
        progress.setVisibility(View.VISIBLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    // Named by the package, so a copy of another one never resumes.
                    String build = (apk.optString("built_utc") + apk.optString("build"))
                            .replaceAll("[^0-9A-Fa-f]", "");
                    File file = new File(getCacheDir(), "lavender-shift-update-"
                            + build.substring(0, Math.min(32, build.length())) + ".apk");
                    File[] old = getCacheDir().listFiles();
                    for (int i = 0; old != null && i < old.length; i++) {
                        String name = old[i].getName();
                        if (name.startsWith("lavender-shift-update-") && !name.startsWith(file.getName())) {
                            old[i].delete();
                        }
                    }
                    long size = apk.optLong("bytes");
                    if (file.length() != size) {
                        fetch(code, API + "apk", file, size, new long[] {0}, size, 1, 1,
                                System.currentTimeMillis());
                    }
                    commitUpdate(file);
                    finish("The installer asks to confirm the update. The app closes while it updates; "
                            + "open it again afterwards.", true);
                } catch (IOException error) {
                    Log.e(TAG, "update failed", error);
                    finish("The update stopped: " + error.getMessage() + ". Press Install update to try again.",
                            false);
                }
            }
        }, "AppUpdate").start();
    }

    // The package into a system installer session; the system asks the
    // player to confirm, through INSTALL_STATUS.
    private void commitUpdate(File file) throws IOException {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(getPackageName());
        params.setSize(file.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = session.openWrite("lavender-shift.apk", 0, file.length())) {
                copy(in, out, null);
                session.fsync(out);
            }
            Intent status = new Intent(INSTALL_STATUS).setPackage(getPackageName());
            // Mutable: the installer adds the result to it.
            PendingIntent callback = PendingIntent.getBroadcast(this, id, status,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(callback.getIntentSender());
        } catch (IOException | RuntimeException error) {
            installer.abandonSession(id);
            throw error instanceof IOException ? (IOException) error : new IOException(error.getMessage());
        }
        file.delete();
    }

    private void installStatus(Intent status) {
        int result = status.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (result == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = status.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
            if (confirm != null) {
                try {
                    startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                } catch (Exception error) {
                    progressText.setText("The system installer did not open: " + error.getMessage());
                }
            }
            return;
        }
        if (result == PackageInstaller.STATUS_SUCCESS) {
            progressText.setText("Updated. Open Lavender Shift again.");
            return;
        }
        String message = status.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        progressText.setText(result == PackageInstaller.STATUS_FAILURE_ABORTED
                ? "The update was cancelled."
                : "The update was not installed" + (message != null ? ": " + message : "") + ".");
    }

    // A path from the manifest, inside the app's folder: the game or the user folder.
    private File destination(String path) throws IOException {
        if (!(path.startsWith("game/disc/") || path.startsWith("game/update/") || path.startsWith("user/"))
                || path.contains("\\")) {
            throw new IOException("unexpected path " + path);
        }
        for (String part : path.split("/")) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                throw new IOException("unexpected path " + path);
            }
        }
        File target = new File(base, path);
        if (!target.getCanonicalPath().startsWith(base.getCanonicalPath() + File.separator)) {
            throw new IOException("unexpected path " + path);
        }
        return target;
    }

    // One file into <name>.part, resumed from its length, then renamed. A
    // dropped connection is retried a few times.
    private void fetch(String code, String resource, File target, long size, final long[] done, final long total,
                       final int number, final int count, final long started) throws IOException {
        File part = new File(target.getPath() + ".part");
        target.getParentFile().mkdirs();
        final long before = done[0];
        IOException last = null;
        for (int attempt = 0; attempt < 6; attempt++) {
            if (attempt > 0) {
                try {
                    Thread.sleep(2000L * attempt);
                } catch (InterruptedException interrupted) {
                    throw new IOException("interrupted");
                }
            }
            long offset = part.length();
            if (offset > size) {
                part.delete();
                offset = 0;
            }
            try {
                HttpURLConnection connection = open("http://" + server + resource, code, offset);
                int status = connection.getResponseCode();
                if (status == 403) throw new RefusedException(readError(connection));
                if (status != 200 && status != 206) throw new IOException("the PC answered " + status);
                boolean append = status == 206;
                if (!append) offset = 0;
                final long startOffset = offset;
                try (InputStream in = connection.getInputStream();
                     OutputStream out = new FileOutputStream(part, append)) {
                    copy(in, out, new Progress() {
                        @Override
                        public void written(long bytes) {
                            done[0] = before + startOffset + bytes;
                            report(done[0], total, number, count, started);
                        }
                    });
                }
                if (part.length() != size) throw new IOException("incomplete download");
                if (target.exists() && !target.delete()) throw new IOException("cannot replace " + target);
                if (!part.renameTo(target)) throw new IOException("cannot write " + target);
                done[0] = before + size;
                return;
            } catch (RefusedException refused) {
                throw refused;
            } catch (IOException error) {
                last = error;
                Log.w(TAG, "retrying " + target, error);
            }
        }
        throw last;
    }

    private long lastReport;

    private void report(long done, long total, int number, int count, long started) {
        long now = System.currentTimeMillis();
        if (now - lastReport < 250) return;
        lastReport = now;
        double seconds = Math.max(1, (now - started) / 1000.0);
        final String line = size(done) + " of " + size(total) + " · file " + number + " of " + count
                + " · " + size((long) (done / seconds)) + "/s";
        final int value = total > 0 ? (int) (done * 1000 / total) : 0;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                progress.setProgress(value);
                progressText.setText(line);
            }
        });
    }

    // ---- Backups --------------------------------------------------------

    // The device's save (each profile folder in user but the shared content,
    // and the achievements), moved into user/backups before the PC's
    // replaces it.
    // A save copy that stopped resumes without a second backup: what is in
    // place then is the PC's, partly copied.
    private void backUpSave(String stamp) throws IOException {
        File marker = new File(base, SAVE_INCOMPLETE);
        if (marker.exists()) return;
        File user = new File(base, "user");
        File[] entries = user.listFiles();
        marker.getParentFile().mkdirs();
        if (entries == null) {
            new FileOutputStream(marker).close();
            return;
        }
        File backup = new File(base, "user/backups/before-pc-save-" + stamp);
        int moved = 0;
        for (File entry : entries) {
            String name = entry.getName();
            boolean profile = entry.isDirectory() && name.matches("[0-9A-Fa-f]{16}") && !name.equals(SHARED_PROFILE);
            if (!profile && !name.equals("achievements")) continue;
            File target = new File(backup, name);
            target.getParentFile().mkdirs();
            if (!entry.renameTo(target)) throw new IOException("cannot move " + entry + " to the backup");
            moved++;
        }
        new FileOutputStream(marker).close();
        if (moved > 0) later(progressText, "This device's save was moved to " + backup);
    }

    // The device's settings file, copied into user/backups before the PC's
    // replaces it.
    private void backUpSettings(String stamp) throws IOException {
        File settings = new File(base, SETTINGS_FILE);
        if (!settings.isFile()) return;
        File backup = new File(base, "user/backups/before-pc-settings-" + stamp + "/" + settings.getName());
        backup.getParentFile().mkdirs();
        try (InputStream in = new FileInputStream(settings); OutputStream out = new FileOutputStream(backup)) {
            copy(in, out, null);
        }
        // Removed so that a copy that stops is never mistaken for the PC's.
        if (!settings.delete()) throw new IOException("cannot replace " + settings);
        later(progressText, "This device's settings were backed up to " + backup.getParent());
    }

    // ---- Launch arguments -----------------------------------------------

    private boolean hasArguments(String[] flags) {
        File file = new File(base, LavenderShiftActivity.ARGUMENTS_FILE);
        if (!file.isFile()) return false;
        try (InputStream in = new FileInputStream(file)) {
            List<String> lines = new ArrayList<>();
            for (String line : readAll(in).split("\n")) lines.add(line.trim());
            for (String flag : flags) {
                if (!lines.contains(flag)) return false;
            }
            return true;
        } catch (IOException error) {
            return false;
        }
    }

    // Adds or removes flags in the arguments file, keeping its other lines.
    private void setArguments(String[] flags, boolean on) throws IOException {
        File file = new File(base, LavenderShiftActivity.ARGUMENTS_FILE);
        file.getParentFile().mkdirs();
        List<String> lines = new ArrayList<>();
        if (file.isFile()) {
            try (InputStream in = new FileInputStream(file)) {
                for (String line : readAll(in).split("\n")) {
                    String trimmed = line.trim();
                    if (trimmed.isEmpty()) continue;
                    boolean flag = false;
                    for (String known : flags) flag |= trimmed.equals(known);
                    if (!flag) lines.add(trimmed);
                }
            }
        }
        if (on) Collections.addAll(lines, flags);
        try (OutputStream out = new FileOutputStream(file)) {
            out.write((String.join("\n", lines) + "\n").getBytes(StandardCharsets.UTF_8));
        }
    }

    // The PC refused the request (a wrong code, or sharing locked): not retried.
    private static final class RefusedException extends IOException {
        RefusedException(String message) {
            super(message);
        }
    }

    interface Progress {
        void written(long bytes);
    }

    private static void copy(InputStream in, OutputStream out, Progress progress) throws IOException {
        byte[] buffer = new byte[1 << 17];
        long written = 0;
        for (int read; (read = in.read(buffer)) > 0; ) {
            out.write(buffer, 0, read);
            written += read;
            if (progress != null) progress.written(written);
        }
    }

    // ---- Copy from a folder -----------------------------------------

    private void chooseFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        try {
            startActivityForResult(intent, REQUEST_FOLDER);
        } catch (Exception error) {
            progressText.setText("This device has no folder picker.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_FOLDER || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri tree = data.getData();
        busy = true;
        refresh();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        progress.setVisibility(View.VISIBLE);
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    importFolder(tree);
                    finish("Done. Press Play.", true);
                } catch (IOException error) {
                    Log.e(TAG, "folder import failed", error);
                    finish("The copy stopped: " + error.getMessage() + ". Choose the folder again to continue.",
                            false);
                }
            }
        }, "FolderImport").start();
    }

    private static final class Entry {
        final Uri uri;
        final String path;
        final long size;

        Entry(Uri uri, String path, long size) {
            this.uri = uri;
            this.path = path;
            this.size = size;
        }
    }

    // The PC's .local\game folder (disc and update in it), or its disc
    // folder alone (default.xex in it).
    private void importFolder(Uri tree) throws IOException {
        String root = DocumentsContract.getTreeDocumentId(tree);
        List<Entry> found = new ArrayList<>();
        list(tree, root, "", found);
        boolean whole = false;
        boolean discOnly = false;
        for (Entry entry : found) {
            whole |= entry.path.equals("disc/default.xex");
            discOnly |= entry.path.equals("default.xex");
        }
        if (!whole && !discOnly) {
            throw new IOException("that folder holds neither disc\\default.xex nor default.xex; choose the "
                    + "PC's .local\\game folder or its disc folder");
        }
        List<Entry> files = new ArrayList<>();
        List<String> targets = new ArrayList<>();
        long total = 0;
        for (Entry entry : found) {
            String target;
            if (discOnly) {
                target = "game/disc/" + entry.path;
            } else if (entry.path.startsWith("disc/") || entry.path.startsWith("update/")) {
                target = "game/" + entry.path;
            } else {
                continue;  // the PC's catalogs and generated code
            }
            files.add(entry);
            targets.add(target);
            total += entry.size;
        }
        if (total > base.getUsableSpace() - (256L << 20)) {
            throw new IOException("not enough free space: " + size(total) + " needed");
        }
        File incomplete = new File(base, INCOMPLETE);
        incomplete.getParentFile().mkdirs();
        new FileOutputStream(incomplete).close();
        long done = 0;
        final long started = System.currentTimeMillis();
        final int count = files.size();
        for (int i = 0; i < files.size(); i++) {
            Entry entry = files.get(i);
            File target = destination(targets.get(i));
            if (target.length() != entry.size) {
                File part = new File(target.getPath() + ".part");
                target.getParentFile().mkdirs();
                final long before = done;
                final int number = i + 1;
                final long sum = total;
                try (InputStream in = getContentResolver().openInputStream(entry.uri);
                     OutputStream out = new FileOutputStream(part)) {
                    if (in == null) throw new IOException("cannot read " + entry.path);
                    copy(in, out, new Progress() {
                        @Override
                        public void written(long bytes) {
                            report(before + bytes, sum, number, count, started);
                        }
                    });
                }
                if (part.length() != entry.size) throw new IOException("incomplete copy of " + entry.path);
                if (target.exists() && !target.delete()) throw new IOException("cannot replace " + target);
                if (!part.renameTo(target)) throw new IOException("cannot write " + target);
            }
            done += entry.size;
        }
        incomplete.delete();
    }

    private void list(Uri tree, String document, String prefix, List<Entry> out) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, document);
        try (Cursor cursor = getContentResolver().query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE}, null, null, null)) {
            if (cursor == null) throw new IOException("cannot list the folder");
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                        || name.contains("/")) {
                    continue;
                }
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    list(tree, id, prefix + name + "/", out);
                } else {
                    out.add(new Entry(DocumentsContract.buildDocumentUriUsingTree(tree, id), prefix + name,
                            cursor.getLong(3)));
                }
            }
        }
    }
}
