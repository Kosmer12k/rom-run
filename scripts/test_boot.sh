#!/system/bin/sh
R=/data/user/0/com.engine.container/files/rootfs
echo "=== Test Real Zygote Boot WITH -Xzygote AND --start-system-server ==="
export BOOTCLASSPATH="/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:/system/framework/framework.jar:/system/framework/framework-graphics.jar:/system/framework/framework-location.jar:/system/framework/framework-connectivity-b.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/system/framework/framework-platformcrashrecovery.jar:/system/framework/framework-ondeviceintelligence-platform.jar:/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.adservices/javalib/framework-adservices.jar:/apex/com.android.adservices/javalib/framework-sdksandbox.jar:/apex/com.android.appsearch/javalib/framework-appsearch.jar:/apex/com.android.btservices/javalib/framework-bluetooth.jar:/apex/com.android.configinfrastructure/javalib/framework-configinfrastructure.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar:/apex/com.android.devicelock/javalib/framework-devicelock.jar:/apex/com.android.healthfitness/javalib/framework-healthfitness.jar:/apex/com.android.ipsec/javalib/android.net.ipsec.ike.jar:/apex/com.android.media/javalib/updatable-media.jar:/apex/com.android.mediaprovider/javalib/framework-mediaprovider.jar:/apex/com.android.mediaprovider/javalib/framework-pdf.jar:/apex/com.android.mediaprovider/javalib/framework-pdf-v.jar:/apex/com.android.mediaprovider/javalib/framework-photopicker.jar:/apex/com.android.nfcservices/javalib/framework-nfc.jar:/apex/com.android.ondevicepersonalization/javalib/framework-ondevicepersonalization.jar:/apex/com.android.os.statsd/javalib/framework-statsd.jar:/apex/com.android.permission/javalib/framework-permission.jar:/apex/com.android.permission/javalib/framework-permission-s.jar:/apex/com.android.profiling/javalib/framework-profiling.jar:/apex/com.android.scheduling/javalib/framework-scheduling.jar:/apex/com.android.sdkext/javalib/framework-sdkextensions.jar:/apex/com.android.tethering/javalib/framework-connectivity.jar:/apex/com.android.tethering/javalib/framework-connectivity-t.jar:/apex/com.android.tethering/javalib/framework-tethering.jar:/apex/com.android.uwb/javalib/framework-uwb.jar:/apex/com.android.virt/javalib/framework-virtualization.jar:/apex/com.android.wifi/javalib/framework-wifi.jar"
export SYSTEMSERVERCLASSPATH="/system/framework/com.android.location.provider.jar:/system/framework/services.jar:/apex/com.android.adservices/javalib/service-adservices.jar:/apex/com.android.adservices/javalib/service-sdksandbox.jar:/apex/com.android.appsearch/javalib/service-appsearch.jar:/apex/com.android.art/javalib/service-art.jar:/apex/com.android.configinfrastructure/javalib/service-configinfrastructure.jar:/apex/com.android.healthfitness/javalib/service-healthfitness.jar:/apex/com.android.media/javalib/service-media-s.jar:/apex/com.android.ondevicepersonalization/javalib/service-ondevicepersonalization.jar:/apex/com.android.permission/javalib/service-permission.jar:/apex/com.android.rkpd/javalib/service-rkp.jar:/apex/com.android.virt/javalib/service-virtualization.jar"
export DEX2OATBOOTCLASSPATH="/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:/system/framework/framework.jar:/system/framework/framework-graphics.jar:/system/framework/framework-location.jar:/system/framework/framework-connectivity-b.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/system/framework/framework-platformcrashrecovery.jar:/system/framework/framework-ondeviceintelligence-platform.jar:/apex/com.android.i18n/javalib/core-icu4j.jar"
export ANDROID_ROOT=/system
export ANDROID_DATA="${R}/data"
export LD_PRELOAD=/data/user/0/com.engine.container/files/libzygote_shim.so
unset LD_LIBRARY_PATH

echo "Spawning full Zygote + system_server with -Xzygote..."
/data/local/tmp/zygote_launcher ${R}/system/bin/app_process64 -Xzygote -Xallowinmemorycompilation /system/bin --zygote --start-system-server &
ZYGOTE_PID=$!
echo "Zygote PID: $ZYGOTE_PID"

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    if kill -0 $ZYGOTE_PID 2>/dev/null; then
        echo "[$i] ZYGOTE IS RUNNING! (PID: $ZYGOTE_PID)"
    else
        echo "[$i] Zygote exited."
        break
    fi
done
