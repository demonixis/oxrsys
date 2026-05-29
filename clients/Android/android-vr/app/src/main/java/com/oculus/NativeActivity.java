// SPDX-License-Identifier: MPL-2.0

package com.oculus;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import java.util.HashMap;

/**
 * Wrapper around android.app.NativeActivity for Meta Quest VR runtime.
 *
 * The Quest runtime checks the activity class name to determine VR focus handling.
 * Using com.oculus.NativeActivity matches the pattern used by Meta's own SDK samples.
 */
public class NativeActivity extends android.app.NativeActivity
{
    private static final String TAG = "OXRSys-NativeActivity";
    private static final String ACTION_USB_PERMISSION = "net.demonixis.oxrsys.android.USB_PERMISSION";

    private UsbManager usbManager;
    private PendingIntent usbPermissionIntent;
    private boolean usbReceiverRegistered = false;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver()
    {
        @Override
        public void onReceive(Context context, Intent intent)
        {
            String action = intent.getAction();
            if (ACTION_USB_PERMISSION.equals(action))
            {
                handleUsbPermissionResult(intent);
            }
            else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action) ||
                     UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(action))
            {
                Log.i(TAG, "USB attach intent received: " + action);
                probeUsbPermissionTargets(intent);
            }
            else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action) ||
                     UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(action))
            {
                Log.i(TAG, "USB detach intent received: " + action);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        Log.i(TAG, "onCreate");
        initializeUsbPermissionHandling();
        super.onCreate(savedInstanceState);
        probeUsbPermissionTargets(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent)
    {
        Log.i(TAG, "onNewIntent");
        super.onNewIntent(intent);
        setIntent(intent);
        probeUsbPermissionTargets(intent);
    }

    @Override
    protected void onResume()
    {
        Log.i(TAG, "onResume");
        super.onResume();
        probeUsbPermissionTargets(getIntent());
    }

    @Override
    protected void onPause()
    {
        Log.i(TAG, "onPause");
        super.onPause();
    }

    @Override
    protected void onDestroy()
    {
        Log.i(TAG, "onDestroy");
        if (usbReceiverRegistered)
        {
            unregisterReceiver(usbReceiver);
            usbReceiverRegistered = false;
        }
        super.onDestroy();
    }

    private void initializeUsbPermissionHandling()
    {
        usbManager = (UsbManager)getSystemService(Context.USB_SERVICE);
        Intent permissionIntent = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
        int pendingIntentFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
        {
            pendingIntentFlags |= PendingIntent.FLAG_MUTABLE;
        }
        usbPermissionIntent = PendingIntent.getBroadcast(this, 0, permissionIntent, pendingIntentFlags);

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
        {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        }
        else
        {
            registerReceiver(usbReceiver, filter);
        }
        usbReceiverRegistered = true;
    }

    private void probeUsbPermissionTargets(Intent launchIntent)
    {
        if (usbManager == null || usbPermissionIntent == null)
        {
            Log.w(TAG, "UsbManager unavailable; cannot request USB permission");
            return;
        }

        boolean requestedPermission = false;
        UsbDevice attachedDevice = launchIntent != null
            ? launchIntent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            : null;
        if (attachedDevice != null)
        {
            requestedPermission |= requestDevicePermissionIfNeeded(attachedDevice, "intent");
        }

        UsbAccessory attachedAccessory = launchIntent != null
            ? launchIntent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY)
            : null;
        if (attachedAccessory != null)
        {
            requestedPermission |= requestAccessoryPermissionIfNeeded(attachedAccessory, "intent");
        }

        HashMap<String, UsbDevice> devices = usbManager.getDeviceList();
        if (devices != null)
        {
            for (UsbDevice device : devices.values())
            {
                requestedPermission |= requestDevicePermissionIfNeeded(device, "enumeration");
            }
        }

        UsbAccessory[] accessories = usbManager.getAccessoryList();
        if (accessories != null)
        {
            for (UsbAccessory accessory : accessories)
            {
                requestedPermission |= requestAccessoryPermissionIfNeeded(accessory, "enumeration");
            }
        }

        if (!requestedPermission)
        {
            Log.i(TAG, "No UsbManager device/accessory target needs permission. "
                    + "ADB reverse USB streaming does not require an app USB permission dialog.");
        }
    }

    private boolean requestDevicePermissionIfNeeded(UsbDevice device, String source)
    {
        Log.i(TAG, "USB device from " + source + ": name=" + device.getDeviceName()
                + " vendor=0x" + Integer.toHexString(device.getVendorId())
                + " product=0x" + Integer.toHexString(device.getProductId())
                + " class=" + device.getDeviceClass());
        if (usbManager.hasPermission(device))
        {
            Log.i(TAG, "USB device permission already granted: " + device.getDeviceName());
            return false;
        }

        Log.i(TAG, "Requesting USB device permission: " + device.getDeviceName());
        usbManager.requestPermission(device, usbPermissionIntent);
        return true;
    }

    private boolean requestAccessoryPermissionIfNeeded(UsbAccessory accessory, String source)
    {
        Log.i(TAG, "USB accessory from " + source + ": manufacturer=" + accessory.getManufacturer()
                + " model=" + accessory.getModel()
                + " version=" + accessory.getVersion());
        if (usbManager.hasPermission(accessory))
        {
            Log.i(TAG, "USB accessory permission already granted: " + accessory.getModel());
            return false;
        }

        Log.i(TAG, "Requesting USB accessory permission: " + accessory.getModel());
        usbManager.requestPermission(accessory, usbPermissionIntent);
        return true;
    }

    private void handleUsbPermissionResult(Intent intent)
    {
        boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
        UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        UsbAccessory accessory = intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
        if (device != null)
        {
            Log.i(TAG, "USB device permission result: granted=" + granted
                    + " name=" + device.getDeviceName());
        }
        else if (accessory != null)
        {
            Log.i(TAG, "USB accessory permission result: granted=" + granted
                    + " model=" + accessory.getModel());
        }
        else
        {
            Log.i(TAG, "USB permission result without device/accessory: granted=" + granted);
        }
    }
}
