#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include "fat32.h"

#define FAT_START 16384
#define DATA_START 829440
#define CLUSTER_SIZE 1024

using namespace std;

vector<string> MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

enum nodeType {_FILE, _FOLDER, _DOT};

vector<unsigned> EOC = {248, 255, 255, 15};

char* imgFile;

vector<string> tokenizeString(string s, char delimeter) {
    vector<string> tokens;
    string current;
    for (auto& character : s) {
        if (character == delimeter) {
            tokens.push_back(current);
            current.erase();
        } else {
            current.push_back(character);
        }
    }
    if (current.size()) {
        tokens.push_back(current);
    }
    return tokens;
}

vector<string> extractDirectories(string path) {
    vector<string> directories;
    string dirBuffer;
    if (path[0] == '/') {
        directories.push_back("/");
        path = path.substr(1);
    }
    for (auto& dir : path) {
        if (dir == '/') {
            directories.push_back(dirBuffer);
            dirBuffer.erase();
        } else {
            dirBuffer.push_back(dir);
        }
    }
    if (dirBuffer.size() > 0) {
        directories.push_back(dirBuffer);
    }
    return directories;
}

class FileNode {
public:
    string name;
    string realName;
    FileNode* realNode;
    bool isRead;
    enum nodeType type;
    vector<unsigned>* clusterChain;
    FileNode* parentRef;
    unsigned firstClusterIndex;
    vector<FileNode*> children;
    FatFileEntry* entry;
    int order;
    uint16_t binaryModifiedDate;
    unsigned short modifiedDay;
    unsigned short modifiedYear;
    string modifiedMonth;
    uint16_t binaryModifiedTime;
    unsigned short modifiedHour;
    unsigned short modifiedMinute;
    unsigned short modifiedSecond;
    unsigned fileSize;
    FileNode() {
        isRead = false;
        clusterChain = nullptr;
        parentRef = nullptr;
        firstClusterIndex = 0;
        entry = nullptr;
        realNode = nullptr;
        order = 0;
    }
    FileNode(string name, bool isRead, enum nodeType type, vector<unsigned>* clusterChain, FileNode* parentRef, unsigned firstClusterIndex, vector<FileNode*> children, FatFileEntry* entry) :
        name(name),
        isRead(isRead),
        type(type),
        clusterChain(clusterChain),
        parentRef(parentRef),
        children(children),
        entry(entry),
        firstClusterIndex(firstClusterIndex)
    {}
    FileNode(const FileNode& base) {
        isRead = base.isRead;
        type = base.type;
        clusterChain = base.clusterChain;
        parentRef = base.parentRef;
        firstClusterIndex = base.firstClusterIndex;
        children = base.children;
        entry = base.entry;
        order = base.order;
        binaryModifiedDate = base.binaryModifiedDate;
        modifiedDay = base.modifiedDay;
        modifiedMonth = base.modifiedMonth;
        modifiedYear = base.modifiedYear;
        binaryModifiedTime = base.binaryModifiedTime;
        modifiedHour = base.modifiedHour;
        modifiedMinute = base.modifiedMinute;
        modifiedSecond = base.modifiedSecond;
    }
    void setModifiedDate(uint16_t date) {
        binaryModifiedDate = date;
        modifiedYear = 1980 + (date >> 9);
        uint16_t mask = 480; 
        modifiedMonth = MONTHS[((date & mask) >> 5) - 1];
        uint16_t mask2 = 31;
        modifiedDay = date & mask2;
    }
    void setModifiedTime(uint16_t time) {
        binaryModifiedTime = time;
        modifiedHour = time >> 11;
        uint16_t mask = 2016; // 00000 111111 00000 
        modifiedMinute = (time & mask) >> 5;
        uint16_t mask2 = 31; // 00000 000000 11111
        modifiedSecond = (time & mask2) * 2;
    }
    void setModifiedTimeAndYear(int year, int month, int day, int hour, int minute, int second) {
        modifiedYear = year + 1900;
        modifiedMonth = MONTHS[month - 1];
        modifiedDay = day;
        modifiedHour = hour;
        modifiedMinute = minute;
        modifiedSecond = second;
    }
};

FileNode** fileTree = new FileNode*;

vector<unsigned>* getClusterChain(uint32_t firstClusterIndex) {
    cout << "getting chain starts " << (int) firstClusterIndex << endl;
    vector<unsigned>* clusterChain = new vector<unsigned>;
    clusterChain->push_back(firstClusterIndex);
    if (firstClusterIndex == 0) {
        return clusterChain;
    }
    FILE* fp = fopen(imgFile, "r");
    uint8_t* currentByte = new uint8_t;
    fseek(fp, FAT_START + firstClusterIndex * 4, SEEK_SET);
    while (1) {
        vector<unsigned> fatEntry;
        unsigned entryValue;
        for (int j = 0; j < 4; j++) {
            fread(currentByte, sizeof(uint8_t), 1, fp);
            fatEntry.push_back(unsigned(*currentByte));
        }
        if (fatEntry == EOC) {
            break;
        }
        entryValue = fatEntry[0] + (fatEntry[1] << 2) + (fatEntry[2] << 4) + (fatEntry[3] << 8);
        clusterChain->push_back(entryValue);
    }
    delete currentByte;
    fclose(fp);
    return clusterChain;
}

void getFileAndFolders(FileNode* root) {
    FILE* fp = fopen(imgFile, "r");
    vector<unsigned>* clusterChain = root->clusterChain;
    for (auto& clusterIndex : *clusterChain) {
        cout << "in cluster " << clusterIndex << endl;
        unsigned offset = DATA_START + clusterIndex * CLUSTER_SIZE;
        fseek(fp, offset, SEEK_SET);
        string concatLfn = "";
        for (int i = 0; i < 32; i++) { // read 1024 bytes
            FatFileEntry* fatFile = new FatFileEntry; // memleak
            string name83;
            fread(fatFile, sizeof(FatFileEntry), 1, fp);
            unsigned attributes = fatFile->msdos.attributes;
            if (attributes == 15) { // LFN entry
                string lfnName;
                for (int j = 0; j < 5; j++) {
                    if (!isascii(fatFile->lfn.name1[j]) || fatFile->lfn.name1[j] == 0 || fatFile->lfn.name1[j] == 32) {
                        break;
                    }
                    lfnName.push_back((char) fatFile->lfn.name1[j]);
                }
                for (int j = 0; j < 6; j++) {
                    if (!isascii(fatFile->lfn.name2[j]) || fatFile->lfn.name2[j] == 0 || fatFile->lfn.name2[j] == 32) {
                        break;
                    }
                    lfnName.push_back((char) fatFile->lfn.name2[j]);
                }
                for (int j = 0; j < 2; j++) {
                    if (!isascii(fatFile->lfn.name3[j]) || fatFile->lfn.name3[j] == 0 || fatFile->lfn.name3[j] == 32) {
                        break;
                    }
                    lfnName.push_back((char) fatFile->lfn.name3[j]);
                }
                concatLfn = lfnName + concatLfn;
            } else if (attributes == 16 || attributes == 32) { // 8.3 entry
                for (int j = 0; j < 8; j++) {
                    if (!isascii(fatFile->msdos.filename[j]) || fatFile->msdos.filename[j] == 0 || fatFile->msdos.filename[j] == 32) {
                        break;
                    }
                    name83.push_back(fatFile->msdos.filename[j]);
                }
                if (concatLfn.size()) { // end of LFN
                    uint32_t firstCluster = (fatFile->msdos.eaIndex << 16) + fatFile->msdos.firstCluster - 2;
                    if (firstCluster == -2) {
                        cout << "cluster - 2 "<< endl;
                    }
                    FileNode* newNode = new FileNode;
                    newNode->name = concatLfn;
                    cout << "adding new node " << newNode->name << " belongs to " << root->name << endl;
                    newNode->parentRef = root;
                    newNode->isRead = false;
                    newNode->type = attributes == 16 ? _FOLDER : _FILE;
                    newNode->firstClusterIndex = firstCluster;
                    newNode->clusterChain = getClusterChain(firstCluster);
                    newNode->entry = fatFile;
                    newNode->order = name83[1] - '0';
                    newNode->setModifiedDate(fatFile->msdos.modifiedDate);
                    newNode->setModifiedTime(fatFile->msdos.modifiedTime);
                    newNode->fileSize = fatFile->msdos.fileSize;
                    root->children.push_back(newNode);
                    concatLfn.erase();
                } else {
                    time_t t = time(nullptr);
                    struct tm tm = *localtime(&t);
                    if (name83 == ".") {
                        FileNode* fptr = new FileNode(*root);
                        fptr->name = ".";
                        fptr->realName = root->name;
                        fptr->realNode = root;
                        fptr->type = _DOT;
                        // fptr->setModifiedTimeAndYear(tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                        cout << "adding . belongs to " << root->name << endl;
                        root->children.push_back(fptr);
                    } else if (name83 == "..") {
                        FileNode* fptr = new FileNode(*(root->parentRef));
                        fptr->name = "..";
                        fptr->realName = root->parentRef->name;
                        fptr->realNode = root->parentRef;
                        fptr->type = _DOT;
                        // fptr->setModifiedTimeAndYear(tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                        cout << "adding .. belongs to " << root->name << endl;
                        root->children.push_back(fptr);
                    }
                } 
            }
            delete fatFile;
        }
    }
    fclose(fp);
}

void createTree(FileNode* root) {
    if (root->type == _FOLDER) {
        cout << "creating tree root -> " << root->name << endl;
        getFileAndFolders(root);
        root->isRead = true;
        for (auto& child : root->children) {
            createTree(child);
        }
    }
}

FileNode* findDirectory(FileNode* currentDir, vector<string>& directories) {
    bool found = false;
    if (directories.size() == 0) {
        return nullptr;
    }
    if (directories[0] == "/") { // absolute path
        currentDir = *fileTree;
        directories.erase(directories.begin());
        if (directories.size() == 0) {
            return currentDir;
        }
    }
    for (auto& child : currentDir->children) {
        if (child->name == directories[0]) {
            found = true;
            if (child->type == _FOLDER) {
                if (directories.size() == 1) {
                    return child;
                }
                vector<string> newVector(directories.begin() + 1, directories.end());
                return findDirectory(child, newVector);
            } else if (child->type == _DOT) {
                if (directories.size() == 1) {
                    return child->realNode;
                }
                vector<string> newVector(directories.begin() + 1, directories.end());
                return findDirectory(child->realNode, newVector);
            }
        }
    }
    if (!found) {
        return nullptr;
    }
}

string findAbsolutePath(FileNode* file) { // USE FOR FOLDERS
    if (file->name == "/") {
        return "/";
    }
    string path = "";
    deque<string> names;
    while (file->parentRef != nullptr) {
        names.push_front(file->name);
        file = file->parentRef;
    }
    for (int i = 0; i < names.size() - 1; i++) {
        path = path + names[i];
        path.push_back('/');
    }
    path += names[names.size() - 1];
    return "/" + path;
}

int main(int argc, char** argv) {
    // Bytes per sector = 512
    // cluster size = 1024 bytes
    // Sectors per FAT = 794
    // #FATs = 2
    // Sectors per cluster = 2
    // # reserved sectors = 32
    // start byte of fat = 16384
    // FATs size = 2 * 794 * 512 = 813056
    // data section start = 829440
    
    imgFile = argv[1];
    BPB_struct* bpb = new BPB_struct;
    BPB32_struct* bpb32 = new BPB32_struct;
    FatFile83* fat83 = new FatFile83;
    FatFileEntry* fatFile = new FatFileEntry;
    FatFileLFN* fatLFN = new FatFileLFN;
    void* vp = ((void*) bpb) + 36;
    bpb32 = (BPB32_struct*) vp;
    cout << "root cluster = " << ((int) bpb32->RootCluster) << endl;
    FILE* fp = fopen(argv[1], "r");
    FILE* fp2 = fopen(argv[1], "r");
    fseek(fp, 829440, SEEK_SET);
    fseek(fp2, 16384, SEEK_SET);

    uint8_t** fatEntry = new uint8_t*[100];
    for (int i = 0; i < 100; i++) {
        fatEntry[i] = new uint8_t[4];
        printf("0x%X: ", i);
        for (int j = 0; j < 4; j++) {
            fread(&fatEntry[i][j], sizeof(uint8_t), 1, fp2);
            printf("0x%X ", fatEntry[i][j]);
        }
        cout << endl;
    }
    cout << endl;
    string concatName;

    for (int i = 0; i < 32; i++) { // ROOT DIRECTORY - CLUSTER 2
        char* name = new char[13];
        char* extension = new char[3];
        char* shortName = new char[7];
        uint8_t sequenceNumber;
        uint8_t firstByte;
        fread(fatFile, sizeof(FatFileEntry), 1, fp);
        unsigned attributes = fatFile->msdos.attributes;
        if (attributes == 15) { // LFN entry
            string current;
            for (int j = 0; j < 5; j++) {
                if (!isascii(fatFile->lfn.name1[j]) || fatFile->lfn.name1[j] == 0 || fatFile->lfn.name1[j] == 32) {
                    break;
                }
                current.push_back((char) fatFile->lfn.name1[j]);
            }
            for (int j = 0; j < 6; j++) {
                if (!isascii(fatFile->lfn.name2[j]) || fatFile->lfn.name1[j] == 0 || fatFile->lfn.name2[j] == 32) {
                    break;
                }
                current.push_back((char) fatFile->lfn.name2[j]);
            }
            for (int j = 0; j < 2; j++) {
                if (!isascii(fatFile->lfn.name3[j]) || fatFile->lfn.name1[j] == 0 || fatFile->lfn.name3[j] == 32) {
                    break;
                }
                current.push_back((char) fatFile->lfn.name3[j]);
            }
            concatName = current + concatName;
            cout << "name = " << concatName << "\t\t\t\t\t";
            printf("sequence no = 0x%X ", fatFile->lfn.sequence_number);
            cout << "checksum = " << (int) fatFile->lfn.checksum << endl;
        } else if (attributes == 16 || attributes == 32) { // 8.3 entry
            concatName.erase();
            for (int j = 0; j < 8; j++) {
                if (!isascii(fatFile->msdos.filename[j]) || fatFile->msdos.filename[j] == 0 || fatFile->msdos.filename[j] == 32) {
                    break;
                }
                if (j == 0) {
                    printf("adding char[0] 0x%X\n", fatFile->msdos.filename[j]);
                } else {
                    cout << "adding a char -> " << fatFile->msdos.filename[j] << " int value = " << (int) fatFile->msdos.filename[j] << endl;
                }
                name[j] = fatFile->msdos.filename[j];
            }
            cout << "name = " << name << "\t\t\t\t\t";
        }
        printf("attributes = 0x%X ", fatFile->msdos.attributes);
        printf("first cluster 0-1 = 0x%X 0x%x ", fatFile->msdos.eaIndex, fatFile->msdos.firstCluster);
        printf("modified date = %u ", fatFile->msdos.modifiedDate);
        printf("modified time = %u ", fatFile->msdos.modifiedTime);
        cout << endl;
        delete[] name;
        delete[] extension;
        delete[] shortName;
    }
    fclose(fp);
    cout << "END OF CLUSTER 2 " << endl;
    cout << endl;
    /*
    char* content = new char[1024];
    for (int i = 0; i < 30; i++) {
        cout << "content in cluster " << i + 3 << endl;
        if (i == 4 || i== 0) {
            for (int k = 0; k < 32; k++) {
                char* name = new char[13];
                char* extension = new char[3];
                char* shortName = new char[7];
                uint8_t sequenceNumber;
                uint8_t firstByte;
                fread(fatFile, sizeof(FatFileEntry), 1, fp);
                unsigned attributes = fatFile->msdos.attributes;
                if (attributes == 15) {
                    for (int j = 0; j < 5; j++) {
                        name[j] = fatFile->lfn.name1[j];
                    }
                    for (int j = 0; j < 6; j++) {
                        name[j + 5] = fatFile->lfn.name2[j];
                    }
                    for (int j = 0; j < 2; j++) {
                        name[j + 11] = fatFile->lfn.name3[j];
                    }
                    cout << "name = " << name << "\t\t\t\t\t";
                    printf("sequence no = 0x%X ", fatFile->lfn.sequence_number);

                } else {
                    for (int j = 0; j < 8; j++) {
                        if (j == 0) {
                            printf("adding char[0] 0x%X\n", fatFile->msdos.filename[j]);
                        } else {
                            cout << "adding a char -> " << fatFile->msdos.filename[j] << endl;
                        }
                        name[j] = fatFile->msdos.filename[j];
                    }
                    cout << "name = " << name << "\t\t\t\t\t";
                }
                printf("attributes = 0x%X ", fatFile->msdos.attributes);
                printf("first cluster 0-1 = 0x%X 0x%x", fatFile->msdos.eaIndex, fatFile->msdos.firstCluster);
                cout << endl;
            }
        } else {
            fread(content, sizeof(char), 1024, fp);
            cout << content << endl;
        }
    }
    */
    string pwd = "/";
    FileNode* root = new FileNode;
    root->name = "/";
    root->clusterChain = getClusterChain(bpb32->RootCluster); // will have memleak;
    root->firstClusterIndex = bpb32->RootCluster;
    root->type = _FOLDER;
    root->isRead = false;
    createTree(root);
    *fileTree = root;
    parsed_input* input = new parsed_input;
    string line;
    FileNode* currentDir = root;
    while (1) {
        cout << pwd << "> ";
        getline(cin, line);
        vector<string> command = tokenizeString(line, ' ');
        if (command[0] == "quit") {
            break;
        } else if (command[0] == "cd") {
            string arg1 = string(command[1]);
            vector<string> directories = extractDirectories(arg1);
            FileNode* directory = findDirectory(currentDir, directories);
            if (directory != nullptr) {
                pwd = findAbsolutePath(directory);
                currentDir = directory;
            }
        } else if (command[0] == "ls") {
            FileNode* listedDirectory = currentDir;
            if (command.size() > 1 && command[1] == "-l") {
                if (command.size() > 2) { // ls -l <path>
                    vector<string> directories = extractDirectories(command[2]);
                    FileNode* directory = findDirectory(currentDir, directories);
                    if (directory == nullptr) {
                        continue;
                    }
                    listedDirectory = directory;
                }
                for (auto* child : listedDirectory->children) {
                    if (child->type == _FOLDER || child->type == _DOT) {
                        cout << "drwx------ 1 root root 0 ";
                    } else {
                        cout << "-rwx------ 1 root root ";
                    }
                    cout << child->modifiedYear << " " << child->modifiedMonth << " " << child->modifiedDay
                    << " " << child->modifiedHour << ":" << child->modifiedMinute << " " << child->name << endl;
                }
            } else { // ls <path> or ls
                if (command.size() > 1) {
                    vector<string> directories = extractDirectories(command[1]);
                    FileNode* directory = findDirectory(currentDir, directories);
                    if (directory == nullptr) {
                        continue;
                    }
                    listedDirectory = directory;
                }
                for (auto& child : listedDirectory->children) {
                    cout << child->name << " ";
                }
                cout << endl;
            }
        }

    }

    
    return 0;
}
