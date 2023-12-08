# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(SRCS 
"main.c"
"ad.c"
"wind.c"
"mqtt.c"
"mac_address.c"
"console.c"
                    INCLUDE_DIRS ".")