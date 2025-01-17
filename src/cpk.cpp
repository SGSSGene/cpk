#include "degxlog.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <json.hpp>
#include <vector>
#include <fstream>
#include <regex>

#include "cpk_structs.h"
#include "download.h"
#include "archive.h"
#include "global.h"
#include "thread_pool.h"
#include "os.h"
#include "tools.h"
#include "version.h"
#include "display.h"

namespace cpk
{

std::map <std::string, int> CpkBuildTypes = {
    {"cmake", CMAKE},
};

std::map <std::string, int> CpkLanguages = {
    {"cpp", CPP},
    {"c", C},
    {"rust", RUST},
    {"python", PYTHON},
    {"javascript", JAVASCRIPT},
};

bool InstallPackages(const std::vector<CPKPackage>& packages)
{
    if(packages.size() == 0)
        return false;

    InstallBuildTools();

    nlohmann::json json;
    json["packages"] = {};
    for (const auto& package : packages) {
        json["packages"][package.package] = (package.version.length() > 0) ? package.version : "";
    }
    std::string jsonRequest = json.dump(4);

    std::string response = SendPostRequest(GetRemoteBackend() + "/install", jsonRequest.c_str());
    if (response.length() == 0) {
        DX_ERROR("json", "no respoce from server");
        return false;
    }
    DX_DEBUG("install", "responce: %s", response.c_str());

    nlohmann::json response_json;
    try {
        response_json = nlohmann::json::parse(response);
    } catch(...) {
        DX_ERROR("json", "error parse or respoce from server");
        return false;
    }
    if (response_json.contains("error"))
    {
        bool error = response_json["error"];
        switch ((int)response_json["errorCode"]) {
            default:
                std::string errorDesc = response_json["errorDesc"];
                DX_ERROR("install", "error install: %s", errorDesc.c_str());
        }
        return false;
    }

    std::string cpkDir = ".cpk";
    if (!IsDir(cpkDir))
        MkDir(cpkDir);

    std::vector<int> packages_percent;
    std::mutex packages_status_lock;
    std::vector<std::string> packages_names;
    std::vector<std::string> packages_versions;
    std::vector<std::string> progress_messages;
    packages_percent.resize(response_json["packages"].size());
    packages_names.resize(response_json["packages"].size());
    packages_versions.resize(response_json["packages"].size());
    progress_messages.resize(response_json["packages"].size());

    thread_pool pool;
    const int processor_count = std::thread::hardware_concurrency();
    pool.start(processor_count);

    std::mutex wait_install_mutex;
    std::unordered_map<std::string, std::condition_variable> need_install_deps_conditions;
    std::unordered_map<std::string, bool> need_install_deps_map_ready;
    std::unordered_map<std::string, bool> need_install_deps_map_failed;
    int package_index = -1;
    InitRenderProgressBars(response_json["packages"].size());
    for(const auto& package : response_json["packages"])
    {
        package_index++;
        packages_names[package_index] = package["package"];
        packages_versions[package_index] = package["version"];
        pool.queue([package, 
            cpkDir, 
            processor_count,
            package_index,
            &packages_percent,
            &packages_names,
            &packages_versions,
            &progress_messages,
            &packages_status_lock,
            &wait_install_mutex, 
            &need_install_deps_conditions, 
            &need_install_deps_map_ready,
            &need_install_deps_map_failed
        ]() 
        {
            std::string package_name = package["package"];
            std::string package_version = package["version"];
            std::string package_url = package["url"];
            std::string build_type = package["buildType"];
            std::string package_language = package["language"];

            auto RenderProgress = [&](int percent, bool force = false, const std::string& message = std::string()){
                if (DX_DEBUG_LEVEL() == DX_LEVEL_DEBUG)
                    return; // don't draw progress in debug mode
                std::lock_guard<std::mutex> lock(packages_status_lock);
                packages_percent[package_index] = percent;
                progress_messages[package_index] = message;
                RenderProgressBars(
                    packages_names,
                    packages_versions,
                    packages_percent, 
                    force, 
                    progress_messages, 
                    need_install_deps_map_ready, 
                    need_install_deps_map_failed
                );
            };

            nlohmann::json installedFileSave;
            if (IsExists(cpkDir + "/packages.json"))
            {
                std::ifstream ifs(cpkDir + "/packages.json");
                installedFileSave = nlohmann::json::parse(ifs);
            }
            else
            {
                installedFileSave["packages"] = {};
            }

            bool is_update = false;

            if (installedFileSave["packages"].contains(package_name))
            {
                if (installedFileSave["packages"][package_name]["version"] == package_version)
                {
                    std::lock_guard<std::mutex> lock(packages_status_lock);
                    need_install_deps_map_ready[package_name] = true;
                    DX_DEBUG("install", "%s package installed. Skip.", package_name.c_str());
                    return;   
                }
                else
                {
                    DX_DEBUG("install", "%s package version %s != %s. Update.", 
                        package_name.c_str(), 
                        std::string(installedFileSave["packages"][package_name]["version"]).c_str(), 
                        package_version.c_str()
                    );
                    is_update = true;
                }
            }

            DX_DEBUG("pkg", "wait deps install for package %s", package_name.c_str());
            if (package.contains("dependencies"))
            {
                for(const auto& dep : package["dependencies"].items())
                {
                    DX_DEBUG("pkg", "dep %s", dep.key().c_str());
                    RenderProgress(0, true, "Awaiting " + dep.key() + " package");
                    std::unique_lock lock_install(wait_install_mutex);
                    need_install_deps_conditions[dep.key()].wait(lock_install, [&need_install_deps_map_ready, &dep, &packages_status_lock]{
                        std::lock_guard<std::mutex> lock(packages_status_lock);
                        return need_install_deps_map_ready[dep.key()];
                    });
                }
            }

            RenderProgress(5, true, "Download package...");

            DX_DEBUG("pkg", "install package %s", package_name.c_str());
            DX_DEBUG("install", "download package %s", package_url.c_str());
            std::string zipFile = cpkDir + "/" + package_name + ".zip";
            DownloadFile(package_url.c_str(), zipFile.c_str());
            DX_DEBUG("install", "downloaded %s as %s", package_url.c_str(), zipFile.c_str());
            RenderProgress(15, true, "Unpack...");
            std::string packageDir = cpkDir + "/" + package_name;
            if (is_update)
            {
                Remove(packageDir);
            }
            if (!IsExists(packageDir))
                MkDir(packageDir);
            UnZip(zipFile, packageDir);
            RenderProgress(20, true, "Configure package...");
            DX_DEBUG("install", "Prepared package, start building...");
            switch(CpkBuildTypes[build_type])
            {
                case CMAKE:
                    {
                        DX_DEBUG("install", "build package %s with cmake", package_name.c_str());
                        std::string buildDir = packageDir + "/build";
                        std::string installDir = buildDir + "/install";
                        MkDir(buildDir);
                        MkDir(installDir);
                        installDir = AbsolutePath(installDir);
                        DX_DEBUG("install", "prepare cmake build");
#ifdef CPK_OS_WIN
                        std::string cmake_build_type = "-G \"MinGW Makefiles\"";
#else
                        std::string cmake_build_type = "";
#endif
                        std::string cmake_deps_dirs = "";
                        if (package.contains("dependencies"))
                        {
                            cmake_deps_dirs += " -DCMAKE_PREFIX_PATH=\"";
                            int idx = 0;
                            for(const auto& dep : package["dependencies"].items())
                            {
                                DX_DEBUG("cmake", "dep %s", dep.key().c_str());
                                std::string depDir = cpkDir + "/" + dep.key() + "/build/install";
                                depDir = AbsolutePath(depDir);
                                cmake_deps_dirs += (idx == 0) ? depDir : ";" + depDir;
                                idx++;
                            }
                            cmake_deps_dirs += "\"";
                        }

                        std::string cmake_configure = "cd \"" + packageDir + "\" && cmake -B \"build\" " + cmake_build_type + " -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=\"" + installDir + "\"" + cmake_deps_dirs;
                        bool cmake_configure_result = (DX_DEBUG_LEVEL() == DX_LEVEL_DEBUG) ? EXE(cmake_configure) : EXES(cmake_configure);
                        if (!cmake_configure_result)
                        {
                            packages_status_lock.lock();
                            need_install_deps_map_failed[package_name] = true;
                            packages_status_lock.unlock();
                            RenderProgress(99, true, "failed to conf package");
                            DX_ERROR("install", "failed to prepare project for build");
                            return;
                        }
                        RenderProgress(25, true, "Initialize package build");
                        DX_DEBUG("install", "build");
                        if(!EXEWithPrint("cd \"" + packageDir + "\" && cmake --build \"build\" -j" + std::to_string(processor_count), [&](const std::string& line){
                            DX_DEBUG("cmake", "%s", line.c_str());
                            if (DX_DEBUG_LEVEL() == DX_LEVEL_DEBUG)
                            {
                                return;
                            }
                            std::regex re("\\[\\s+([0-9]+)\\%\\]");
                            std::smatch match;
                            if (std::regex_search(line, match, re))
                            {
                                int percent = std::stoi(match[1].str());
                                percent = 25 + ((float)percent / 100 * 70);
                                RenderProgress(percent, false, "Building package...");
                            }
                        }))
                        {
                            packages_status_lock.lock();
                            need_install_deps_map_failed[package_name] = true;
                            packages_status_lock.unlock();
                            RenderProgress(99, true, "failed to build package");
                            DX_ERROR("install", "failed to build package");
                            return;
                        }
                        RenderProgress(96, true, "Install package");
                        std::string cmake_install = "cd \"" + packageDir + "\" && cmake --install \"build\"";
                        bool cmake_install_result = (DX_DEBUG_LEVEL() == DX_LEVEL_DEBUG) ? EXE(cmake_install) : EXES(cmake_install);
                        if (!cmake_install_result)
                        {
                            packages_status_lock.lock();
                            need_install_deps_map_failed[package_name] = true;
                            packages_status_lock.unlock();
                            RenderProgress(99, true, "failed to install package");
                            DX_ERROR("install", "failed to install package");
                            return;
                        }

                        RenderProgress(99, true, "saving package info");
                    }
                    break;
                default:
                    DX_ERROR("install", "no build type assotiated founded");
            }
            wait_install_mutex.lock();

            packages_status_lock.lock();
            need_install_deps_map_ready[package_name] = true;
            packages_status_lock.unlock();

            if (IsExists(cpkDir + "/packages.json"))
            {
                std::ifstream ifs(cpkDir + "/packages.json");
                installedFileSave = nlohmann::json::parse(ifs);
            }
            else
            {
                installedFileSave["packages"] = {};
            }
            installedFileSave["packages"][package_name] = package;
            std::ofstream installedFile(cpkDir + "/packages.json");
            installedFile << std::setw(4) << installedFileSave << std::endl;
            DX_DEBUG("install", "written package.json");
            SendPostRequest(GetRemoteBackend() + "/installed", "{\"package\": \"" + package_name + "\", \"success\": true}");
            DX_DEBUG("install", "post statistic after installation");

            RenderProgress(100, true, " ");

            wait_install_mutex.unlock();
            need_install_deps_conditions[package_name].notify_all();
        });
    }
    pool.stop();

    bool some_package_failed = false;
    bool some_package_success = false;
    printf("\nSuccessfully installed: ");
    int installed_index = -1;
    for(const auto& package : response_json["packages"])
    {
        installed_index++;
        if (need_install_deps_map_ready[package["package"]])
        {
            some_package_success = true;
            if (response_json["packages"].size() - 1 != installed_index)
                printf(DX_COLOR_GREEN "%s %s, " DX_COLOR_CLEAR, std::string(package["package"]).c_str(), std::string(package["version"]).c_str());
            else
                printf(DX_COLOR_GREEN "%s %s\n" DX_COLOR_CLEAR, std::string(package["package"]).c_str(), std::string(package["version"]).c_str());
        }
        else
        {
            some_package_failed = true;
        }
    }
    if (!some_package_success)
    {
        printf("(none)\n");
    }
    if (some_package_failed)
    {
        installed_index = -1;
        printf("\nFailed to install: ");
        for(const auto& package : response_json["packages"])
        {
            installed_index++;
            if (!need_install_deps_map_ready[package["package"]])
            {
                if (response_json["packages"].size() - 1 != installed_index)
                    printf(DX_COLOR_RED "%s %s, " DX_COLOR_CLEAR, std::string(package["package"]).c_str(), std::string(package["version"]).c_str());
                else
                    printf(DX_COLOR_RED "%s %s\n" DX_COLOR_CLEAR, std::string(package["package"]).c_str(), std::string(package["version"]).c_str());
            }
        }
    }
    return !some_package_failed;
}

void PublishPacket()
{  
    std::string packageName;
    nlohmann::json cpkConfig;
    bool cpkConfigExists = false;

    if (!IsExists(CpkShareDir()))
        MkDir(CpkShareDir());

    if (!IsExists(CpkShareDir() + "/passkey"))
    {
        WriteToFile(CpkShareDir() + "/passkey", GenerateRandomString(128));
    }
    if (!IsExists(CpkShareDir() + "/passkey"))
    {
        DX_ERROR("publish", "no passkey file for publish package");
        return;
    }
    std::string passkey = ReadFileString(CpkShareDir() + "/passkey");
    if (passkey.empty())
    {
        DX_ERROR("publish", "passkey empty");
        return;
    }

    if (IsExists("cpk.json"))
    {
        DX_DEBUG("publish", "found cpk.json");
        std::ifstream ifs("cpk.json");
        cpkConfig = nlohmann::json::parse(ifs);
        packageName = cpkConfig["package"];
        cpkConfigExists = true;
    }
    else
    {
        DX_DEBUG("publish", "cpk.json not founded");
        packageName = Cwd();
        if (packageName.length() == 0) {
            DX_ERROR("publish", "no package name");
            return;
        }
        std::replace( packageName.begin(), packageName.end(), '\\', '/');
        std::size_t foundLastPathSep = packageName.find_last_of("/\\");
        if (foundLastPathSep >= 0 && foundLastPathSep < packageName.length()) {
            packageName = packageName.substr(foundLastPathSep+1);
            if (packageName.length() == 0) {
                DX_ERROR("publish", "no package name");
                return;
            }
        } else {
            DX_ERROR("publish", "no package name");
            return;
        }
    }

    nlohmann::json json = cpkConfig;
    json["package"] = packageName;
    json["language"] = "cpp";
    if (IsExists("CMakeLists.txt")) {
        json["buildType"] = "cmake";
    }

    // manual input
    if (!cpkConfigExists)
    {
        json["package"] = ConsoleInput("package name ?", json["package"]);
        json["description"] = ConsoleInput("description ?");
        json["author"] = ConsoleInput("your name / nick ?");
        json["email"] = ConsoleInput("your email ?");
        json["version"] = ConsoleInput("version ?");
    }

    if (!json.contains("buildType"))
    {
        DX_ERROR("publish", "build type is not detected");
        return;
    }

    json["passkey"] = passkey;

    auto all_files = AllFiles();
    std::string tmpFile = GetTempDir() + "/temp.zip";
    DX_DEBUG("publish", "generation temp.zip");
    CreateZip(all_files, tmpFile);
    DX_DEBUG("publish", "generated temp.zip");

    FILE* in_file = fopen(tmpFile.c_str(), "rb");
    if (!in_file) {
        DX_ERROR("zip", "cant open created zip");
        fclose(in_file);
        Remove(tmpFile);
        return;
    }

    size_t in_size = FileSize(tmpFile);
    const char* archive_content = (char*)malloc(in_size);
    fread((void*)archive_content, in_size, 1, in_file);

    DX_DEBUG("publish", "send %d", in_size);

    std::string jsonRequest = json.dump(4);
    std::string response = SendPostZip(GetRemoteBackend() + "/publish", jsonRequest, archive_content, in_size);
    DX_DEBUG("publish", "response %s", response.c_str());
    fclose(in_file);
    Remove(tmpFile);
    nlohmann::json response_json;
    try {
        response_json = nlohmann::json::parse(response);
    } catch(...) {
        DX_ERROR("json", "error parse or respoce from server");
        return;
    }
    
    json.erase("passkey");
    
    if (response_json.contains("error"))
    {
        bool error = response_json["error"];
        switch ((int)response_json["errorCode"]) {
            default:
                std::string errorDesc = response_json["errorDesc"];
                DX_ERROR("publish", "error publish: %s", errorDesc.c_str());
        }
        return;
    }
    else
    {
        DX_INFO("publish", "publish successfull");
        WriteToFile("cpk.json", jsonRequest);
    }
}

void PackagesList()
{
    std::string response = SendPostRequest(GetRemoteBackend() + "/packages", "{}");
    DX_DEBUG("packages", "response %s", response.c_str());
    nlohmann::json response_json;
    try {
        response_json = nlohmann::json::parse(response);
    } catch(...) {
        DX_ERROR("json", "error parse or respoce from server");
        return;
    }
    if (response_json.contains("error"))
    {
        bool error = response_json["error"];
        switch ((int)response_json["errorCode"]) {
            default:
                std::string errorDesc = response_json["errorDesc"];
                DX_ERROR("install", "error install: %s", errorDesc.c_str());
        }
        return;
    }

    printf("packages:\n");
    for(const auto& package : response_json["packages"])
    {
        std::string package_name = package["package"];
        std::string package_version = package["version"];
        printf(" %s: %s\n", package_name.c_str(), package_version.c_str());
    }
}


char* getCmdOption(char ** begin, char ** end, const std::string & option)
{
    char ** itr = std::find(begin, end, option);
    if (itr != end && ++itr != end)
    {
        return *itr;
    }
    return 0;
}

bool cmdOptionExists(char** begin, char** end, const std::string& option, bool clear = false)
{
    char ** itr = std::find(begin, end, option);
    bool found = (itr != end);
    if (clear && found)
    {
        (*itr)[0] = '\0';
    }
    return found;
}

void printLogo()
{
printf(
"___________________ ____  __.\n"
"\\_   ___ \\______   \\    |/ _|\n"
"/    \\  \\/|     ___/      <  \n"
"\\     \\___|    |   |    |  \\ \n"
" \\______  /____|   |____|__ \\\n"
"        \\/                 \\/\n"
);
}

void printHelp()
{
    printf("./cpk [command] - CPK package manage\n");
    printf("commands:\n");
    printf("  install package1 [package2[@version]] - install package1, package2 and other\n");
    printf("  publish - publish current package\n");
    printf("  packages - list avaiable packages\n");
    printf("  update - update all installed packages for current project\n");
    printf("  [build command] - build project used installed libraries. As example you can use:\n");
    printf("                    \"cpk cmake -G Ninja ../\" instead of \"cmake -G Ninja ../\" for your project\n");
    printf("  --version / -v - version of CPK\n");
}

int cpk_main(int argc, char *argv[]) {
    int retVal = 0;
    if (!IsExists(CpkShareDir()))
        MkDir(CpkShareDir());
    DX_LOGWRITTER_START((CpkShareDir() + "/cpk.log").c_str());

#ifdef CPK_RELEASE
    if (cmdOptionExists(argv, argv+argc, "--debug", true) || cmdOptionExists(argv, argv+argc, "-d", true))
    {
        DX_SET_DEBUG_LEVEL(3);
    }
#else
    if (!cmdOptionExists(argv, argv+argc, "-nd", true))
    {
        DX_SET_DEBUG_LEVEL(3);
    }
#endif

    char version[] = GIT_DESCRIBE;
    char revision[] = "r" GIT_REVISION;

    if (cmdOptionExists(argv, argv+argc, "--version") || cmdOptionExists(argv, argv+argc, "-v")) {
        printf("%s", version);
        return 0;
    }

    DX_INFO("cpk", "%s %s [os: %s %s]", version, revision, cpk::GetOSType().c_str(), cpk::GetOSArch().c_str());

    printLogo();

    if(argc == 1 || cmdOptionExists(argv, argv+argc, "-h"))
    {
        printHelp();
        return 0;
    }

    if (cmdOptionExists(argv, argv+argc, "--release", true)) {
        EnableRemoteBackend();
    }

    if(argc >= 2) {
        if (strcmp(argv[1], "install") == 0) {
            std::vector<CPKPackage> packages;
            for (int i = 2; i < argc; i++)
            {
                CPKPackage package;
                if (strlen(argv[i]) == 0)
                    continue;
                auto packageVer = Split(argv[i], "@");
                package.package = packageVer[0];
                if (packageVer.size() == 2)
                    package.package = packageVer[1];
                packages.push_back(package);
            }
            retVal = InstallPackages(packages) ? 0 : 1;
        } else if (strcmp(argv[1], "publish") == 0) {
                PublishPacket();
        } else if (strcmp(argv[1], "packages") == 0) {
                PackagesList();
        } else if (strcmp(argv[1], "update") == 0) {
            nlohmann::json installedFileSave;
            std::string cpkDir = ".cpk";
            if (IsExists(cpkDir + "/packages.json"))
            {
                std::ifstream ifs(cpkDir + "/packages.json");
                installedFileSave = nlohmann::json::parse(ifs);
                std::vector<CPKPackage> packages;
                for(const auto& package : installedFileSave["packages"])
                {
                    CPKPackage pkg;
                    pkg.package = package["package"];
                    packages.push_back(pkg);
                }
                if (packages.size() > 0)
                {
                    retVal = InstallPackages(packages) ? 0 : 1;
                }
            }
            else
            {
                DX_INFO("update", "nothing to update");
            }
        } else {
            std::string cpkDir = AbsolutePath(".");

            bool stop = false;
            while (!stop && !IsExists(cpkDir + "/.cpk/packages.json"))
            {
                std::string oldDir = cpkDir;
                cpkDir = DirName(cpkDir); // parent dir
                if (oldDir == cpkDir)
                    stop = true;
            }
            if (stop)
            {
                DX_ERROR("cpk", "can't find .cpk dir to execute given command for dir %s", cpkDir.c_str());
                return 1;
            }
            cpkDir += "/.cpk";

            nlohmann::json installedFileSave;
            std::ifstream ifs(cpkDir + "/packages.json");
            installedFileSave = nlohmann::json::parse(ifs);

            if (strcmp(argv[1], "cmake") == 0)
            {
                DX_INFO("cmake", "handle cmake command, adding args");
                std::vector<std::string> args(argv + 1, argv + 1 + (argc - 1));
                
                std::string libPath = "-DCMAKE_PREFIX_PATH=\"";
                int idx = 0;
                for(const auto& package : installedFileSave["packages"])
                {
                    libPath += (idx == 0) ? (cpkDir + "/") : (";" + cpkDir + "/");
                    libPath += std::string(package["package"]);
                    libPath += "/build/install";
                    idx++;
                }
                libPath += "\"";

                args.insert(args.begin() + 1, libPath);
                
                std::string cmakeCmd = Join(args, " ");
                DX_INFO("cmake", "%s", cmakeCmd.c_str());
                retVal = EXE(cmakeCmd) ? 0 : 1;
            }
        }
    }

    DX_LOGWRITTER_END();
    return retVal;
}

}