#ifndef FileSys_h
#define FileSys_h

#ifdef USE_SPIFFS
    #if defined(CONFIG_IDF_TARGET_ESP32C3)
        #include <FS.h>
    #else
        #include <SPIFFS.h>
    #endif
#else
    #include <LittleFS.h>
#endif

// define USE_SPIFFS for SPIFFS, else LittleFS
class FileSys {
    public:
        FileSys();

        // Mount filesystem and read /boot.msg
        bool begin( bool formatOnFail = false );

        // Use this object anywhere a fs::FS object can be used
        operator fs::FS&();

    private:
        fs::FS &_fs;
};

#endif