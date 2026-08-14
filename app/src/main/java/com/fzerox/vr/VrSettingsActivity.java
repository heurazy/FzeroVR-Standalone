package com.fzerox.vr;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.widget.Button;
import android.widget.RadioGroup;
import android.widget.Switch;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public final class VrSettingsActivity extends Activity {
    private static final String PREFS = "fzero_vr_settings";
    private static final String KEY_REFRESH = "refresh_hz";
    private static final String KEY_DIORAMA = "start_diorama";

    private RadioGroup refreshGroup;
    private Switch startDiorama;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_vr_settings);

        refreshGroup = findViewById(R.id.refresh_group);
        startDiorama = findViewById(R.id.switch_start_diorama);
        Button save = findViewById(R.id.button_save_settings);
        Button back = findViewById(R.id.button_back_settings);

        loadPreferences();
        save.setOnClickListener(v -> saveAndClose());
        back.setOnClickListener(v -> finish());
    }

    private void loadPreferences() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        int hz = prefs.getInt(KEY_REFRESH, 72);
        if (hz == 80) refreshGroup.check(R.id.refresh_80);
        else if (hz == 90) refreshGroup.check(R.id.refresh_90);
        else if (hz == 120) refreshGroup.check(R.id.refresh_120);
        else refreshGroup.check(R.id.refresh_72);
        startDiorama.setChecked(prefs.getBoolean(KEY_DIORAMA, false));
    }

    private int selectedRefreshRate() {
        int id = refreshGroup.getCheckedRadioButtonId();
        if (id == R.id.refresh_80) return 80;
        if (id == R.id.refresh_90) return 90;
        if (id == R.id.refresh_120) return 120;
        return 72;
    }

    private void saveAndClose() {
        int hz = selectedRefreshRate();
        boolean diorama = startDiorama.isChecked();
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putInt(KEY_REFRESH, hz)
                .putBoolean(KEY_DIORAMA, diorama)
                .apply();

        String config = "refresh_hz=" + hz + "\nstart_diorama=" + (diorama ? 1 : 0) + "\n";
        File file = new File(getFilesDir(), "vr_settings.cfg");
        try (FileOutputStream out = new FileOutputStream(file, false)) {
            out.write(config.getBytes(StandardCharsets.US_ASCII));
            out.flush();
            out.getFD().sync();
            Toast.makeText(this, R.string.settings_saved, Toast.LENGTH_SHORT).show();
            finish();
        } catch (Exception e) {
            Toast.makeText(this, R.string.settings_save_failed, Toast.LENGTH_LONG).show();
        }
    }
}
