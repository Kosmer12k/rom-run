# Keep native bridge methods and classes
-keepclassmembers class com.engine.container.MainActivity {
    native <methods>;
}
-keep class com.engine.container.** { *; }
