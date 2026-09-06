package com.engine.container

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.IBinder
import android.util.Log

/**
 * Host-side BroadcastReceiver for Virtual Binder Shim.
 *
 * In an unrooted user-space container, the kernel driver /dev/binder is locked
 * by the host OS ServiceManager (which claimed BINDER_SET_CONTEXT_MGR).
 *
 * This receiver serves as the out-of-band IPC registry:
 * 1. ACTION_REGISTER_GUEST_SM: Accepts the guest ServiceManager IBinder token.
 * 2. ACTION_GET_GUEST_SM: Supplies the IBinder token to requesting guest processes
 *    via synchronous Broadcast result extras.
 */
class VirtualBinderReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "VirtualBinderReceiver"

        const val ACTION_REGISTER_GUEST_SM = "com.engine.container.REGISTER_GUEST_SM"
        const val ACTION_GET_GUEST_SM = "com.engine.container.GET_GUEST_SM"
        const val EXTRA_BINDER = "extra_binder"

        @Volatile
        var guestServiceManagerBinder: IBinder? = null
            private set

        fun registerServiceManager(binder: IBinder) {
            guestServiceManagerBinder = binder
            Log.i(TAG, "Virtual ServiceManager token registered on host: $binder")
        }

        fun isServiceManagerReady(): Boolean {
            return guestServiceManagerBinder != null
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        Log.d(TAG, "Received broadcast action: $action")

        when (action) {
            ACTION_REGISTER_GUEST_SM -> {
                val extras = intent.extras
                if (extras != null) {
                    val binder = extras.getBinder(EXTRA_BINDER)
                    if (binder != null) {
                        registerServiceManager(binder)
                        setResultCode(Activity.RESULT_OK)
                        Log.i(TAG, "Successfully registered guest ServiceManager token via broadcast")
                        return
                    }
                }
                Log.e(TAG, "ACTION_REGISTER_GUEST_SM failed: null IBinder extra payload")
                setResultCode(Activity.RESULT_CANCELED)
            }

            ACTION_GET_GUEST_SM -> {
                val binder = guestServiceManagerBinder
                if (binder != null) {
                    val resultBundle = Bundle()
                    resultBundle.putBinder(EXTRA_BINDER, binder)
                    setResultExtras(resultBundle)
                    setResultCode(Activity.RESULT_OK)
                    Log.d(TAG, "Returned guest ServiceManager token to requester")
                } else {
                    Log.w(TAG, "ACTION_GET_GUEST_SM queried but ServiceManager is not yet registered")
                    setResultCode(Activity.RESULT_CANCELED)
                }
            }
        }
    }
}
