#include <string>
#include <vector>
#include <iostream>

#include <cerrno>
#include <fstream>
#include <cstring>
#include <filesystem>

#include <unistd.h>
#include <sys/stat.h>
#include <yaml-cpp/yaml.h>
#include <archive.h>
#include <archive_entry.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <array>
#include <string>
#include <stdexcept>
#include <regex>
#include <cstdio>
#include <curl/curl.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string home_folder = getenv("HOME");
std::string stdpath = home_folder + "/programs";
std::string server_url = "http://127.0.0.1:8000/";

std::string configs = home_folder + "/.config/yapm/";
std::string app_configs = configs + "example.json";
std::string mirrors_config = configs + "mirrors.json";

// print help
void print_help(std::string name_program) {
    std::cout << "Usage:\n";
    std::cout << "Install package: " + name_program + " -S <package_name>" << std::endl;
    std::cout << "Upgrade package: " + name_program + " -U <package_name>" << std::endl;
    std::cout << "Find package: " + name_program + " -Ss <somethink>" << std::endl;
    std::cout << "Install mirrors: " + name_program + " -Sm" << std::endl;
    std::cout << "Make setup: " + name_program + " -Se" << std::endl;
    std::cout << "Upgrade all packages: " + name_program + " -Ua" << std::endl;
}

// is file exists
bool fileExists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}

// is directory exists
bool directoryExists(const std::string dirName) {
    struct stat info;
    if (stat(dirName.c_str(), &info) != 0)
        return false;
    else if (info.st_mode & S_IFDIR)  // S_ISDIR() macro also works
        return true;
    else
        return false;
}

// delete file
bool deleteFile(const std::string& filename) {
    // The remove function returns 0 on success, and a non-zero value on failure.
    return std::remove(filename.c_str()) == 0;
}

// delete directory
void deleteDirectory(const fs::path dirPath) {
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (fs::is_directory(entry)) {
            deleteDirectory(entry);
        } else {
            fs::remove(entry);
        }
    }

    fs::remove(dirPath);
}

// read json from file
json read_json(std::string filename) {
    std::ifstream reading(filename);
    json data = json::parse(reading);
    reading.close();
    return data;
}

// collect keys from json
void collectKeys(const json j, std::vector<std::string>& keys, const std::string prefix = "") {
    for (auto it = j.begin(); it != j.end(); ++it) {
        std::string new_prefix = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
            collectKeys(*it, keys, new_prefix);
        } else {
            keys.push_back(new_prefix);
        }
    }
}

// unpack tar gz
int unpack_targz(std::string filename) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int r;

    // Open the compressed file
    a = archive_read_new();
    archive_read_support_filter_gzip(a);
    archive_read_support_format_tar(a);
    r = archive_read_open_filename(a, filename.c_str(), 10240); // Note: Adjust the block size as needed

    if (r != ARCHIVE_OK) {
        std::cerr << "Failed to open archive." << std::endl;
        return 1;
    }

    // Extract each entry in the archive
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* entry_pathname = archive_entry_pathname(entry);

        // Skip if it's a directory
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            continue;
        }

        // Create the extraction destination file
        std::string dest_filename = "./" + std::string(entry_pathname);
        ext = archive_write_disk_new();
        archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME);
        archive_entry_set_pathname(entry, dest_filename.c_str());
        r = archive_write_header(ext, entry);

        if (r != ARCHIVE_OK) {
            std::cerr << "Failed to write header for " << entry_pathname << std::endl;
            return 1;
        }

        // Read and write data
        char buff[10240]; // Buffer size
        r = ARCHIVE_OK;
        while (r == ARCHIVE_OK) {
            r = archive_read_data(a, buff, sizeof(buff));
            if (r < ARCHIVE_OK) {
                std::cerr << "Failed to read data from " << entry_pathname << std::endl;
                return 1;
            }
            if (r > 0) {
                archive_write_data(ext, buff, r);
            }
        }

        archive_write_finish_entry(ext);
        archive_write_free(ext);
    }

    // Clean up
    archive_read_close(a);
    archive_read_free(a);

    return 0;
}

// Callback function to write data to a file
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* out = static_cast<std::ofstream*>(userp);
    size_t totalSize = size * nmemb;
    out->write(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// progress bar
int ProgressCallback(void* userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    if (dltotal == 0) return 0; // Avoid division by zero

    int progress = static_cast<int>((dlnow * 100) / dltotal);
    int width = 50; // Width of the progress bar

    std::cout << "\r["; // Carriage return to overwrite the line
    for (int i = 0; i < width; ++i) {
        if (i < (progress / 2)) {
            std::cout << "#";
        } else {
            std::cout << " ";
        }
    }
    std::cout << "] " << progress << "%";
    std::cout.flush();

    return 0;
}

int wget(const std::string outputFilename) {
    const std::string url = server_url + outputFilename;

    CURL* curl;
    CURLcode res;

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
        return 1;
    }

    std::ofstream outFile(outputFilename, std::ios::binary);
    if (!outFile) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("Could not open file for writing: " + outputFilename);
        return 1;

    }

    std::cout << "\r[                                                  ] 0%";
    std::cout.flush();

    // Set the URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set the write callback function
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);

    // Pass the file stream to the callback function
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outFile);

    // Set the progress callback function
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Perform the request
    res = curl_easy_perform(curl);

    // Check for errors
    if (res != CURLE_OK) {
        outFile.close();
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
        return 1;
    }

    // Cleanup
    curl_easy_cleanup(curl);
    outFile.close();
    curl_global_cleanup();

    // Move to the next line after the progress bar
    std::cout << std::endl;

    return 0;
}

// question install (yes or no)
bool are_sure(std::string message) {
    std::string response;
    std::cout << message << " [y/n]: ";
    std::getline(std::cin, response);

    for (char& c : response) {
        c = std::tolower(c);
    }

    if (response == "y") {
        return true;
    }

    return false;
}

void exec(std::string cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);

    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    std::cout << result;
}

// install package and build
int install_package(std::string package_name, bool ignore_question, std::string message) {
    if (!ignore_question) {
        if (!are_sure(message)) {
            std::cerr << "Aborted by user." << std::endl;
            return 1;
        }
    }

    int rc;
    std::string package_file = package_name + ".tar.gz";

    // install package
    std::cout << "Downloading archive." << std::endl;
    std::string url = package_file;
    rc = wget(url);
    if (rc) {
        return 1;
    }

    // unpack package
    std::cout << "Unpacking package." << std::endl;
    rc = unpack_targz(package_file);
    if (rc) {
        return 1;
    }

    // delete .tar.gz
    std::cout << "Delete archive." << std::endl;
    fs::remove(package_file);

    // get in to folder
    if (chdir(package_name.c_str())) {
        std::cerr << "Folder for installation wasn't found." << std::endl;
        return 1;
    }

    // get configs (config.yaml)

    std::string build_file = "build.sh";
    std::string build_folder = "build";
    std::vector<std::string> files = {};
    std::vector<std::string> depends = {};

    int is_opensource = 1;

    YAML::Node config = YAML::LoadFile("config.yaml");

    // not optional
    if (config["is_opensource"]) {
        is_opensource = config["is_opensource"].as<int>();
    } if (config["depends"]) {
        depends = config["depends"].as<std::vector<std::string>>();
    } if (config["files"]) {
        files = config["files"].as<std::vector<std::string>>();
    } if (config["build_folder"]) {
        build_folder = config["build_folder"].as<std::string>();
    }

    // optional if opensource
    if (config["build_file"] && is_opensource) {
        build_file = config["build_file"].as<std::string>();
    }

    // install depends
    std::cout << "Download depends." << std::endl;

    if (chdir("..")) {
        return 1;
    }

    for (std::string pack : depends) {
        rc = install_package(pack, true, "");

        if (rc) {
            std::cerr << "Error while installing depends." << std::endl;
            return 1;
        }
    }

    if (chdir(package_name.c_str())) {
        std::cerr << "Folder for installation wasn't found." << std::endl;
        return 1;
    }


    // build package if opensource (if its not its alr compiled)
    if (is_opensource) {
        std::cout << "Build package." << std::endl;
        std::string run_building = "bash " + build_file;
        exec(run_building);
    }

    // move to bin
    std::cout << "Moving package to " << stdpath << "." << std::endl;
    for (std::string file : files) {
        try {
            fs::rename(build_folder + "/" + file, stdpath + "/" + file);
        } catch (fs::__cxx11::filesystem_error error) {
            std::cerr << "Couldn't found binary files in package (package isn't builded correctly)." << std::endl;
            return 1;
        }
    }

    // cd ..
    if (chdir("..")) {
        std::cerr << "Can't exit from build folder." << std::endl;
        return 1;
    }

    // create file (if doesnt exits)

    std::cout << "Update configs." << std::endl;

    if (!fileExists(app_configs)) {
        std::ofstream creation(app_configs);
        creation << "{}";
    }

    // read and update file
    json data = read_json(app_configs);
    data[package_name] = files;

    std::string data_dumped = data.dump(4);

    // write updated file
    std::ofstream input(app_configs, std::ios::trunc | std::ios::out);

    input << data_dumped;

    input.close();

    // delete useless
    deleteDirectory(package_name);

    return 0;
}

// main
int main(int argc, char *argv[]) {
    if (argc == 1) {
        print_help(argv[0]);
    } else if (argc >= 3) {
        std::string flag = argv[1];

        std::vector<std::string> packages_name;

        for (int i = 2; i < argc; i++) {
            std::string package_name = argv[i];
            packages_name.push_back(package_name);
        }

        if (flag == "-S" || flag == "-U") {
            int rc;

            for (int i = 0; i < packages_name.size(); i++) {
                std::cout << packages_name[i];
                if (i != packages_name.size()-1) {
                    std::cout << " , ";
                }
            }

            std::cout << std::endl;

            for (int i = 0; i < packages_name.size(); i++) {
                std::string package_name = packages_name[i];

                if (flag == "-S") {
                    rc = install_package(package_name, i != 0, "Are you sure to install this packages");
                    if (rc) {
                        std::cerr << "Error on installation." << std::endl;
                        return rc;
                    } else {
                        std::cout << "Package was installed successfully." << std::endl;
                    }
                } else {
                    rc = install_package(package_name, i != 0, "Are you sure to update this packages");
                    if (rc) {
                        std::cerr << "Error on updating." << std::endl;
                        return rc;
                    } else {
                        std::cout << "Package was updated successfully." << std::endl;
                    }
                }

                if (i > package_name.size() - 1) {
                    std::cout << std::endl;
                }
            }
        } else if (flag == "-Ss") { // find
            json data = read_json(mirrors_config);
            std::vector<std::string> vec = data["packages"];
            size_t packages_name_size = packages_name.size();

            for (int i = 0; i < packages_name_size; i++) {
                std::string package_name = packages_name[i];

                // Define the regex pattern
                std::string reg = ".*" + package_name + ".*";
                std::regex pattern(reg);
                
                int count = 0;

                // Iterate through the vector and find matches
                for (std::string str : vec) {
                    if (std::regex_match(str, pattern)) {
                        std::cout << str << std::endl;
                        count++;
                    }
                }

                std::cout << "Found " << count << " matches." << std::endl;

                if (i != packages_name_size-1) {
                    std::cout << std::endl;
                }
            }
        } else if (flag == "-R") { // delete

            for (int i = 0; i < packages_name.size(); i++) {
                std::cout << packages_name[i];
                if (i != packages_name.size()-1) {
                    std::cout << " , ";
                }
            }

            std::cout << std::endl;

            if (!are_sure("Are you sure to delete this packages")) {
                std::cerr << "Aborted by user." << std::endl;
                return 1;
            }

            for (int i = 0; i < packages_name.size(); i++) {
                std::string package_name = packages_name[i];

                json data = read_json(app_configs);
                std::vector<std::string> files_to_delete;

                try {
                    files_to_delete = data[package_name];
                } catch (nlohmann::json_abi_v3_11_2::detail::type_error& err) {
                    std::cerr << "Package wasn't found." << std::endl;
                    return 1;
                }

                for (std::string file : files_to_delete) {
                    std::string file_path = stdpath + "/" + file;

                    if (fileExists(file_path)) {
                        if (!deleteFile(file_path)) {
                            std::cerr << "Somethink wet wrong." << std::endl;
                        } else {
                            std::cout << "File: " << file_path << " was deleted successfully." << std::endl;
                        }
                    } else {
                        std::cout << "File: " << file_path << " wasn't found, skiping." << std::endl;
                    }
                }

                data.erase(package_name);

                std::ofstream input(app_configs, std::ios::trunc | std::ios::out);
                std::string data_dumped = data.dump(4);
                input << data_dumped;
                input.close();
            }
        } else {
            print_help(argv[0]);
        }
    } else if (argc == 2) {
        std::string flag = argv[1];

        if (flag == "-Ua") { // update all
            int rc;

            json data = read_json(app_configs);
            std::vector<std::string> packages;
            collectKeys(data, packages);

            if (packages.size() == 0) {
                std::cerr << "No packages was installed, exiting." << std::endl;
                return 0;
            }

            for (int i = 0; i < packages.size(); i++) {
                std::cout << packages[i];
                if (i != packages.size() - 1) {
                    std::cout << " , ";
                } else {
                    std::cout << std::endl;
                }
            }

            for (int i = 0; i < packages.size(); i++) {
                std::string package_name = packages[i];
                rc = install_package(package_name, i != 0, "Are you sure to update this packages");

                if (!rc) {
                    std::cerr << "Error on updating." << std::endl;
                    return rc;
                } else {
                    std::cout << "Package was updated successfully." << std::endl;
                }

                if (i != packages.size() - 1) {
                    std::cout << std::endl;
                }
            }
        } else if (flag == "-Sm") { // install mirrors
            std::string url = "mirrors.json";
            int rc = wget(url);

            if (rc) {
                return 1;
            }

            try {
                fs::rename("mirrors.json", mirrors_config);
            } catch (fs::__cxx11::filesystem_error error) {
                std::cerr << "Couldn't found mirrors.json file." << std::endl;
                return 1;
            }
        } else if (flag == "-Se") { // setup
            std::string path = getenv("PATH");

            if (directoryExists(stdpath) && path.find("~/programs") != std::string::npos) {
                std::cout << "Setup is already finished." << std::endl;
            } else {
                struct stat st;

                std::vector<std::string> needed = {stdpath, home_folder + "/.config",  configs};

                for (std::string i : needed) {
                    if (!directoryExists(i)) {
                        if (mkdir(i.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
                            std::cerr << "Failed to create directory: " << stdpath << " - " << std::strerror(errno) << std::endl;
                        }
                    }
                }

                std::string config_file_path = configs + "setup.sh";
                std::ofstream config_file(config_file_path);

                if (config_file.is_open()) {
                    config_file << "export PATH=\"$PATH:~/programs\"" << std::endl;
                    config_file.close();
                } else {
                    std::cerr << "Unable to open file " << config_file_path << " for writing." << std::endl;
                }

                std::cout << "Setup is done, (please add source " << configs << "setup.sh to your ~/.bashrc then source ~/.bashrc)." << std::endl;
            }
        } else {
            print_help(argv[0]);
        }
    }

    return 0;
}