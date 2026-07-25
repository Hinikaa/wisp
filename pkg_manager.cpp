#include "pkg_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

bool directory_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool create_directory(const std::string& path) {
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool command_exists(const std::string& cmd) {
    std::string check_cmd = "which " + cmd + " > /dev/null 2>&1";
    return system(check_cmd.c_str()) == 0;
}

int run_command(const std::string& cmd) {
    return system(cmd.c_str());
}

std::string get_default_install_dir() {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
    return home + "/.wisp/packages";
}

std::string get_default_manifest_path() {
    return get_default_install_dir() + "/manifest.json";
}

bool is_luarocks_available() {
    return command_exists("luarocks");
}

int install_with_luarocks(const std::string& package_name) {
    std::string cmd = "luarocks install --local " + package_name;
    int result = run_command(cmd);
    if (result == 0) {
        printf("Successfully installed %s with LuaRocks\n", package_name.c_str());
    } else {
        fprintf(stderr, "Failed to install %s with LuaRocks\n", package_name.c_str());
    }
    return result;
}

int install_simple(const std::string& package_name) {
    std::string install_dir = get_default_install_dir();
    create_directory(install_dir);
    
    // For now, create a placeholder module
    // In a real implementation, this would download from a repository
    std::string module_dir = install_dir + "/" + package_name;
    create_directory(module_dir);
    
    std::string init_file = module_dir + "/init.lua";
    std::ofstream init(init_file);
    if (init.is_open()) {
        init << "-- Package: " << package_name << "\n";
        init << "-- Installed by wisp pkg manager\n\n";
        init << "local M = {}\n\n";
        init << "function M.version()\n";
        init << "    return \"1.0.0\"\n";
        init << "end\n\n";
        init << "return M\n";
        init.close();
        printf("Created placeholder package %s\n", package_name.c_str());
        printf("Note: This is a simple fallback installation.\n");
        printf("For full package management, install LuaRocks.\n");
        return 0;
    } else {
        fprintf(stderr, "Failed to create package directory\n");
        return 1;
    }
}

int remove_luarocks(const std::string& package_name) {
    std::string cmd = "luarocks remove --local " + package_name;
    int result = run_command(cmd);
    if (result == 0) {
        printf("Successfully removed %s with LuaRocks\n", package_name.c_str());
    } else {
        fprintf(stderr, "Failed to remove %s with LuaRocks\n", package_name.c_str());
    }
    return result;
}

int remove_simple(const std::string& package_name) {
    std::string install_dir = get_default_install_dir();
    std::string module_dir = install_dir + "/" + package_name;
    
    if (!directory_exists(module_dir)) {
        fprintf(stderr, "Package %s not found\n", package_name.c_str());
        return 1;
    }
    
    std::string cmd = "rm -rf \"" + module_dir + "\"";
    int result = run_command(cmd);
    if (result == 0) {
        printf("Successfully removed %s\n", package_name.c_str());
    } else {
        fprintf(stderr, "Failed to remove %s\n", package_name.c_str());
    }
    return result;
}

std::vector<std::string> list_luarocks() {
    std::vector<std::string> packages;
    FILE* pipe = popen("luarocks list --local 2>/dev/null", "r");
    if (!pipe) return packages;
    
    char buffer[256];
    bool header_passed = false;
    while (fgets(buffer, sizeof buffer, pipe)) {
        std::string line(buffer);
        // Skip header lines
        if (!header_passed) {
            if (line.find("Rocks installed") != std::string::npos) {
                header_passed = true;
                continue;
            }
            continue;
        }
        
        // Skip empty lines and separator lines
        if (line.empty() || line[0] == '\n' || line.find("---") != std::string::npos) {
            continue;
        }
        
        // Parse package lines (format: "name version" or "name version description")
        if (!line.empty() && line[0] != ' ') {
            // Remove newline and split by spaces
            size_t newline_pos = line.find('\n');
            if (newline_pos != std::string::npos) {
                line = line.substr(0, newline_pos);
            }
            
            size_t space_pos = line.find(' ');
            if (space_pos != std::string::npos) {
                packages.push_back(line.substr(0, space_pos));
            } else {
                packages.push_back(line);
            }
        }
    }
    pclose(pipe);
    return packages;
}

std::vector<std::string> list_simple() {
    std::vector<std::string> packages;
    std::string install_dir = get_default_install_dir();
    
    DIR* dir = opendir(install_dir.c_str());
    if (!dir) return packages;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name = entry->d_name;
        std::string path = install_dir + "/" + name;
        if (directory_exists(path)) {
            packages.push_back(name);
        }
    }
    closedir(dir);
    return packages;
}

} // namespace

namespace pkg_manager {

void init() {
    std::string install_dir = get_default_install_dir();
    create_directory(install_dir);
    
    if (is_luarocks_available()) {
        printf("Package manager initialized (using LuaRocks)\n");
    } else {
        printf("Package manager initialized (simple mode - LuaRocks not found)\n");
    }
}

int install(const std::string& package_name) {
    if (package_name.empty()) {
        fprintf(stderr, "Usage: pkg install <package_name>\n");
        return 1;
    }
    
    printf("Installing package: %s\n", package_name.c_str());
    
    if (is_luarocks_available()) {
        return install_with_luarocks(package_name);
    } else {
        return install_simple(package_name);
    }
}

int update(const std::string& package_name) {
    if (package_name.empty()) {
        // Update all packages
        if (is_luarocks_available()) {
            printf("Listing installed packages for update check...\n");
            return run_command("luarocks list --outdated");
        } else {
            printf("Simple mode: update not implemented\n");
            printf("For package updates, install LuaRocks\n");
            return 0;
        }
    } else {
        // Reinstall to update specific package
        if (is_luarocks_available()) {
            printf("Reinstalling %s to update...\n", package_name.c_str());
            return run_command(("luarocks install " + package_name).c_str());
        } else {
            printf("Simple mode: update not implemented for individual packages\n");
            printf("For package updates, install LuaRocks\n");
            return 0;
        }
    }
}

int remove(const std::string& package_name) {
    if (package_name.empty()) {
        fprintf(stderr, "Usage: pkg remove <package_name>\n");
        return 1;
    }
    
    printf("Removing package: %s\n", package_name.c_str());
    
    if (is_luarocks_available()) {
        return remove_luarocks(package_name);
    } else {
        return remove_simple(package_name);
    }
}

int list() {
    std::vector<std::string> packages;
    
    if (is_luarocks_available()) {
        packages = list_luarocks();
    } else {
        packages = list_simple();
    }
    
    if (packages.empty()) {
        printf("No packages installed\n");
        return 0;
    }
    
    printf("Installed packages:\n");
    for (const auto& pkg : packages) {
        printf("  - %s\n", pkg.c_str());
    }
    
    return 0;
}

int search(const std::string& query) {
    if (query.empty()) {
        fprintf(stderr, "Usage: pkg search <query>\n");
        return 1;
    }
    
    if (is_luarocks_available()) {
        std::string cmd = "luarocks search " + query;
        return run_command(cmd);
    } else {
        printf("Simple mode: search not implemented\n");
        printf("For package search, install LuaRocks\n");
        printf("You can manually download packages from:\n");
        printf("  - https://luarocks.org\n");
        printf("  - https://github.com/topics/lua\n");
        return 0;
    }
}

int info(const std::string& package_name) {
    if (package_name.empty()) {
        fprintf(stderr, "Usage: pkg info <package_name>\n");
        return 1;
    }
    
    if (is_luarocks_available()) {
        std::string cmd = "luarocks show " + package_name;
        return run_command(cmd);
    } else {
        std::string install_dir = get_default_install_dir();
        std::string module_dir = install_dir + "/" + package_name;
        
        if (!directory_exists(module_dir)) {
            fprintf(stderr, "Package %s not found\n", package_name.c_str());
            return 1;
        }
        
        printf("Package: %s\n", package_name.c_str());
        printf("Location: %s\n", module_dir.c_str());
        printf("Note: Detailed info not available in simple mode\n");
        return 0;
    }
}

std::string get_install_dir() {
    return get_default_install_dir();
}

std::string get_manifest_path() {
    return get_default_manifest_path();
}

bool is_package_installed(const std::string& package_name) {
    if (is_luarocks_available()) {
        std::string cmd = "luarocks show " + package_name + " > /dev/null 2>&1";
        return run_command(cmd) == 0;
    } else {
        std::string install_dir = get_default_install_dir();
        std::string module_dir = install_dir + "/" + package_name;
        return directory_exists(module_dir);
    }
}

} // namespace pkg_manager