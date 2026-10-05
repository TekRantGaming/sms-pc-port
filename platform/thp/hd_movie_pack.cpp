#include <sms_hd_movies.h>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace {
bool regular_file(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool complete_pack(const std::string& directory)
{
    if (!regular_file(directory + "/sms-hd-cutscenes-v1.complete"))
        return false;
    static const char* movies[] = {
        "autodemoA", "bath", "demogeso", "Entrance", "epilogue", "EX128x144_q0",
        "kagemario", "KuppaJr", "MechaKuppa", "NozuruA", "NozuruB", "omakeA",
        "omakeB", "openingA", "openingBA", "openingBB", "openingBC", "Pakkun",
        "staffroll", "stolennozuru", "stolenpeach"
    };
    for (const char* movie : movies)
        if (!regular_file(directory + "/files/data/" + movie + ".thp"))
            return false;
    return true;
}
}

extern "C" const char* port_hd_cutscene_directory(void)
{
    // HD cutscenes have their own switch (settings.txt hd_cutscenes, the
    // launcher's Graphics page): turning texture packs off leaves them on.
    const char* configured = std::getenv("SMS_HD_CUTSCENES");
    static std::string selected;
    selected.clear();
    if (configured && *configured) {
        if (std::string(configured) != "0" && complete_pack(configured))
            selected = configured;
    } else {
        for (const char* directory : {"mods/hd-cutscenes", "../../mods/hd-cutscenes"})
            if (complete_pack(directory)) {
                selected = directory;
                break;
            }
    }
    return selected.empty() ? nullptr : selected.c_str();
}
