# Applied to apps that use this library, when they shrink with R8.

# JNI: native methods are bound by class and method name.
-keep class dev.vcd.controller.NativeCore {
    native <methods>;
}

# JNI: vccore_jni.c finds the event callbacks by name and signature.
-keep interface dev.vcd.controller.NativeCore$EventSink { *; }
-keepclassmembers class * implements dev.vcd.controller.NativeCore$EventSink {
    public void onConnected(int, int, int);
    public void onFeedback(int, int, int, int, int, int);
    public void onClosed(int, int, java.lang.String);
}
