package com.fzerox.vr;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

public final class FZeroLauncherActivity extends Activity {
    private static final int REQUEST_ROM = 1001;
    private static final String PREFS = "fzero_vr_launcher";
    private static final String KEY_ROM_NAME = "rom_name";
    private static final String ROM_FILE = "baserom.us.rev0.z64";
    private static final int ROM_SIZE = 16 * 1024 * 1024;
    private static final String EXPECTED_SHA1 = "5f658e88ffa9de23cba6986a8fd3d3a90d7b4340";

    private Button buttonLaunch;
    private Button buttonChooseRom;
    private Button buttonChangeRom;
    private TextView textRomName;
    private TextView textRomStatus;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_fzero_launcher);

        buttonLaunch = findViewById(R.id.button_launch);
        buttonChooseRom = findViewById(R.id.button_choose_rom);
        buttonChangeRom = findViewById(R.id.button_change_rom);
        textRomName = findViewById(R.id.text_rom_name);
        textRomStatus = findViewById(R.id.text_rom_status);
        TextView textVersion = findViewById(R.id.text_version);

        buttonLaunch.setOnClickListener(v -> launchGame());
        buttonChooseRom.setOnClickListener(v -> openRomPicker());
        buttonChangeRom.setOnClickListener(v -> openRomPicker());
        findViewById(R.id.button_settings).setOnClickListener(v ->
                startActivity(new Intent(this, VrSettingsActivity.class)));

        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            textVersion.setText(getString(R.string.launcher_version, info.versionName));
        } catch (Exception ignored) {
            textVersion.setText("");
        }

        refreshRomUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        buttonLaunch.setText(R.string.launcher_play);
        refreshRomUi();
    }

    private void openRomPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }

        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }

        final String displayName = getDisplayName(uri);
        setRomCheckingState(displayName);
        new Thread(() -> importRom(uri, displayName), "FZeroRomImport").start();
    }

    private void importRom(Uri uri, String displayName) {
        try (InputStream in = getContentResolver().openInputStream(uri);
             ByteArrayOutputStream memory = new ByteArrayOutputStream(ROM_SIZE)) {
            if (in == null) throw new IllegalArgumentException("Unable to open the selected file.");

            byte[] buffer = new byte[128 * 1024];
            int read;
            int total = 0;
            while ((read = in.read(buffer)) > 0) {
                total += read;
                if (total > ROM_SIZE) {
                    throw new IllegalArgumentException("This is not the expected 16 MiB F-Zero X ROM.");
                }
                memory.write(buffer, 0, read);
            }
            if (total != ROM_SIZE) {
                throw new IllegalArgumentException("This is not the expected 16 MiB F-Zero X ROM.");
            }

            byte[] rom = memory.toByteArray();
            normalizeN64ByteOrder(rom);
            String sha1 = sha1(rom);
            if (!EXPECTED_SHA1.equals(sha1)) {
                throw new IllegalArgumentException("Incompatible ROM. Select F-Zero X (USA) Rev 0.");
            }

            File target = new File(getFilesDir(), ROM_FILE);
            File temp = new File(getFilesDir(), ROM_FILE + ".tmp");
            try (FileOutputStream out = new FileOutputStream(temp, false)) {
                out.write(rom);
                out.flush();
                out.getFD().sync();
            }
            if (target.exists() && !target.delete()) {
                throw new IllegalStateException("Unable to replace the previous ROM.");
            }
            if (!temp.renameTo(target)) {
                throw new IllegalStateException("Unable to install the selected ROM.");
            }

            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_ROM_NAME, displayName).apply();
            runOnUiThread(() -> {
                refreshRomUi();
                Toast.makeText(this, R.string.launcher_rom_ready, Toast.LENGTH_SHORT).show();
            });
        } catch (Exception e) {
            runOnUiThread(() -> {
                refreshRomUi();
                new AlertDialog.Builder(this)
                        .setTitle(R.string.launcher_rom_error_title)
                        .setMessage(e.getMessage() == null ? getString(R.string.launcher_invalid_rom) : e.getMessage())
                        .setPositiveButton(R.string.launcher_choose_another_rom, (dialog, which) -> openRomPicker())
                        .setNegativeButton(android.R.string.cancel, null)
                        .show();
            });
        }
    }

    private static void normalizeN64ByteOrder(byte[] rom) {
        if (rom.length < 4) throw new IllegalArgumentException("ROM is too small.");
        int magic = ((rom[0] & 0xff) << 24) | ((rom[1] & 0xff) << 16) |
                ((rom[2] & 0xff) << 8) | (rom[3] & 0xff);
        if (magic == 0x80371240) {
            return;
        }
        if (magic == 0x37804012) {
            for (int i = 0; i + 1 < rom.length; i += 2) {
                byte t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t;
            }
            return;
        }
        if (magic == 0x40123780) {
            for (int i = 0; i + 3 < rom.length; i += 4) {
                byte a = rom[i];
                byte b = rom[i + 1];
                rom[i] = rom[i + 3];
                rom[i + 1] = rom[i + 2];
                rom[i + 2] = b;
                rom[i + 3] = a;
            }
            return;
        }
        throw new IllegalArgumentException("Invalid Nintendo 64 ROM byte order.");
    }

    private static String sha1(byte[] bytes) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-1");
        byte[] hash = digest.digest(bytes);
        StringBuilder out = new StringBuilder(hash.length * 2);
        for (byte b : hash) out.append(String.format(Locale.US, "%02x", b & 0xff));
        return out.toString();
    }

    private String getDisplayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "F-Zero X ROM" : last;
    }

    private void setRomCheckingState(String displayName) {
        textRomName.setText(displayName);
        textRomStatus.setText(R.string.launcher_rom_checking_description);
        textRomStatus.setTextColor(getColor(R.color.launcher_text_secondary));
        buttonLaunch.setEnabled(false);
        buttonLaunch.setAlpha(0.45f);
        buttonChooseRom.setVisibility(View.GONE);
        buttonChangeRom.setVisibility(View.GONE);
    }

    private void refreshRomUi() {
        File rom = new File(getFilesDir(), ROM_FILE);
        boolean ready = rom.isFile() && rom.length() == ROM_SIZE;
        buttonLaunch.setEnabled(ready);
        buttonLaunch.setAlpha(ready ? 1.0f : 0.45f);
        buttonChooseRom.setVisibility(ready ? View.GONE : View.VISIBLE);
        buttonChangeRom.setVisibility(ready ? View.VISIBLE : View.GONE);

        if (ready) {
            String name = getSharedPreferences(PREFS, MODE_PRIVATE)
                    .getString(KEY_ROM_NAME, "F-Zero X (USA) Rev 0.z64");
            textRomName.setText(name);
            textRomStatus.setText(R.string.launcher_rom_ready);
            textRomStatus.setTextColor(getColor(R.color.launcher_success));
        } else {
            textRomName.setText(R.string.launcher_no_rom);
            textRomStatus.setText(R.string.launcher_no_rom_description);
            textRomStatus.setTextColor(getColor(R.color.launcher_text_secondary));
        }
    }

    private void launchGame() {
        File rom = new File(getFilesDir(), ROM_FILE);
        if (!rom.isFile()) {
            refreshRomUi();
            return;
        }
        buttonLaunch.setEnabled(false);
        buttonLaunch.setText(R.string.launcher_starting);
        Intent intent = new Intent();
        intent.setComponent(new ComponentName(getPackageName(), "android.app.NativeActivity"));
        intent.setAction("com.fzerox.vr.action.PLAY");
        startActivity(intent);
    }
}
