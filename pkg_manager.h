#pragma once

#include <string>
#include <vector>

namespace pkg_manager {

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string repository;
};

// Initialize package manager and set up paths
void init();

// Package operations
int install(const std::string& package_name);
int update(const std::string& package_name = "");
int remove(const std::string& package_name);
int list();
int search(const std::string& query);
int info(const std::string& package_name);

// Utility functions
std::string get_install_dir();
std::string get_manifest_path();
bool is_package_installed(const std::string& package_name);

} // namespace pkg_manager