package com.anlandnext;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/** wayland client closed its own window → daemon broadcast → finish the matching Activity */
public class WindowGoneReceiver extends BroadcastReceiver {
    private static final String TAG = "anland-recv";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!"anland.WINDOW_GONE".equals(intent.getAction())) return;
        long id = intent.getLongExtra("id", -1);
        Log.i(TAG, "window gone broadcast id=" + id);
        WlWindowActivity.finishById(id);
    }
}
