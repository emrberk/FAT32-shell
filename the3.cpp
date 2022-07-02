#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <math.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include "fat32.h"

#define FAT_START 16384
#define FAT2_START 422912
#define FAT_SIZE 406528
#define DATA_START 829440
#define CLUSTER_SIZE 1024

using namespace std;

vector<string> MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

enum nodeType {_FILE, _FOLDER, _DOT};

unsigned EOCVAL = 0x0FFFFFF8;

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

uint8_t getCurrentMs() {
    timeval curTime;
    gettimeofday(&curTime, NULL);
    uint8_t ms = curTime.tv_usec / 1000;
    return ms;
}

uint16_t getCurrentDate() {
    time_t now = time(0);
    tm *tm = localtime(&now);
    uint16_t creationDate = ((tm->tm_year - 80) << 9) + (tm->tm_mon << 5) + tm->tm_mday;
    return creationDate;
}

uint16_t getCurrentTime() {
    time_t now = time(0);
    tm *tm = localtime(&now);
    uint16_t creationTime = (tm->tm_hour << 11) + (tm->tm_min << 5) + tm->tm_sec / 2;
    return creationTime;
}

uint8_t lfn_checksum(char *pFCBName) {
   int i;
   unsigned char sum = 0;
   for (i = 11; i; i--)
      sum = ((sum & 1) << 7) + (sum >> 1) + *pFCBName++;

   return sum;
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
    uint8_t creationMs;
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
        order = 0;
        isRead = base.isRead;
        type = base.type;
        clusterChain = base.clusterChain;
        parentRef = base.parentRef;
        firstClusterIndex = base.firstClusterIndex;
        children = base.children;
        entry = base.entry;
        binaryModifiedDate = base.binaryModifiedDate;
        modifiedDay = base.modifiedDay;
        modifiedMonth = base.modifiedMonth;
        modifiedYear = base.modifiedYear;
        binaryModifiedTime = base.binaryModifiedTime;
        modifiedHour = base.modifiedHour;
        modifiedMinute = base.modifiedMinute;
        modifiedSecond = base.modifiedSecond;
        creationMs = base.creationMs;
    }

    int getMaxOrder() {
        int max = 0;
        for (auto& child : this->children) {
            max = child->order > max ? child->order : max;
        }
        return max;
    }

    void setModifiedDate(uint16_t date) {
        binaryModifiedDate = date;
        modifiedYear = 1980 + (date >> 9);
        uint16_t mask = 480; 
        modifiedMonth = MONTHS[((date & mask) >> 5)];
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

void updateFAT(FileNode* parentDirectory, deque<unsigned> newClusterIndices) {
    FILE* fp = fopen(imgFile, "r+");
    unsigned currentFATStart = FAT_START;
    int i = 0;
    vector<unsigned> parentChain = (*parentDirectory->clusterChain);
    // TODO: Can # of FATs be greater than 2?
    while (i < 2) {
        unsigned firstIndex = parentChain[parentChain.size() - 1];
        for (auto& index : newClusterIndices) {
            fseek(fp, currentFATStart + firstIndex * 4, SEEK_SET);
            uint8_t bytes[4];
            bytes[0] = index & 0xFF;
            bytes[1] = (index & 0xFF00) >> 8;
            bytes[2] = (index & 0xFF0000) >> 16;
            bytes[3] = (index & 0xFF000000) >> 24;
            for (int j = 0; j < 4; j++) {
                fwrite(bytes + j, 1, 1, fp);
            }
            firstIndex = index;
        }
        fseek(fp, currentFATStart + firstIndex * 4, SEEK_SET);
        fwrite(&EOCVAL, 4, 1, fp);
        currentFATStart = FAT2_START;
        i++;
    }
    fclose(fp);
}

bool reserveNewCluster(FileNode* parentDirectory, unsigned remainingEntries) {
    unsigned neededClusters = remainingEntries / 32 + 1;
    FILE* fp = fopen(imgFile, "r");
    deque<unsigned> newClusterIndices;
    // TODO: REMOVE SIZE BASED INDEXING
    for (unsigned i = 2; i < 101598, newClusterIndices.size() < neededClusters; i++) {
        fseek(fp, FAT_START + i * 4, SEEK_SET);
        uint32_t entry;
        fread(&entry, 4, 1, fp);
        if (entry != 0) {
            continue;
        }
        fseek(fp, DATA_START + (i - 2) * CLUSTER_SIZE, SEEK_SET);
        bool fullEmpty = true;
        // TODO: REMOVE SIZE BASED INDEXING
        for (unsigned j = 0; j < 32; j++) {
            FatFileEntry* entry = new FatFileEntry;
            fread(entry, sizeof(FatFileEntry), 1, fp);
            if (entry->msdos.attributes != 0) {
                fullEmpty = false;
                break;
            }
            delete entry;
        }
        if (fullEmpty) {
            newClusterIndices.push_back(i);
        }
    }
    fclose(fp);
    if (newClusterIndices.size() == neededClusters) {
        if (parentDirectory->clusterChain->size() == 0) { // First time creation, for the sake of consistency
            parentDirectory->firstClusterIndex = newClusterIndices[0];
            parentDirectory->clusterChain->push_back(newClusterIndices[0]);
            newClusterIndices.pop_front();
        }
        updateFAT(parentDirectory, newClusterIndices);
        for (auto& index : newClusterIndices) {
            parentDirectory->clusterChain->push_back(index);
        }
        return true;
    }
    return false;
}

vector<unsigned> getAvailableAddresses(FileNode* parentDirectory, unsigned numEntries) {
    bool addressesFound = false;
    vector<unsigned> spaces;
    FILE* fp = fopen(imgFile, "r");
    FatFileEntry* dummyEntry = new FatFileEntry;
    for (auto& cluster : *(parentDirectory->clusterChain)) {
        fseek(fp, DATA_START + (cluster - 2) * CLUSTER_SIZE, SEEK_SET);
        for (unsigned i = 0; i < 32; i++) {
            fread(dummyEntry, sizeof(FatFileEntry), 1, fp);
            if (dummyEntry->msdos.attributes == 0) { // found a space
                spaces.push_back(DATA_START  + (cluster - 2) * CLUSTER_SIZE + i * 32);
            } else {
                spaces.erase(spaces.begin(), spaces.end());
            }
            if (spaces.size() == numEntries) {
                addressesFound = true;
                break;
            }
        }
        if (addressesFound) {
            break;
        }
    }
    delete dummyEntry;
    fclose(fp);
    if (!addressesFound) { // fallback
        unsigned remaining = numEntries - spaces.size();
        bool hasReserved = reserveNewCluster(parentDirectory, remaining);
        // JUST RESERVED SPACES FOR PARENT FOLDER TO CONTAIN THAT FILE ENTRY.
        // FILE SHOULD ALSO HAVE ITS OWN CLUSTERS ...

        // clusters in "spaces" are already available and can be reserved. find places for the remaining entries
        if (hasReserved) {
            return getAvailableAddresses(parentDirectory, numEntries);
        } else { // address & new cluster not found, returns zero sized vector
            spaces.erase(spaces.begin(), spaces.end());
            return spaces;
        }
        /*
        reserving needs to:
            - find an available space in data area if exists
            - update fat table (does it have to update all tables?):
                - find new space in fat table (and store its address ex. 0x0a)
                - write EOC in that address
                - go to the last chain index of fat table and update it with stored address
                - then inform the calling routine about reserved or not
        -- sending arguments to reserveNewCluster will make it too specific.
           it should be used in another context as well.
        */
    }
    return spaces;
}

vector<unsigned>* getClusterChain(uint32_t firstClusterIndex) {
    vector<unsigned>* clusterChain = new vector<unsigned>;
    unsigned currentCluster = firstClusterIndex;
    clusterChain->push_back(currentCluster);
    FILE* fp = fopen(imgFile, "r");
    uint8_t* currentByte = new uint8_t;
    while (1) {
        fseek(fp, FAT_START + currentCluster * 4, SEEK_SET);
        vector<unsigned> fatEntry;
        unsigned entryValue;
        for (int j = 0; j < 4; j++) {
            fread(currentByte, sizeof(uint8_t), 1, fp);
            fatEntry.push_back(unsigned(*currentByte));
        }
        entryValue = fatEntry[0] + (fatEntry[1] << 8) + (fatEntry[2] << 16) + (fatEntry[3] << 24);
        if (entryValue == EOCVAL) {
            break;
        }
        clusterChain->push_back(entryValue);
        currentCluster = entryValue;
    }
    delete currentByte;
    fclose(fp);
    return clusterChain;
}

void getFileAndFolders(FileNode* root) {
    FILE* fp = fopen(imgFile, "r");
    // cout << "getting file and folders for " << root->name << endl;
    // cout << "cluster chain is : ";
    vector<unsigned>* clusterChain = root->clusterChain;
    /*
    for (int i = 0; i < clusterChain->size(); i++) {
        cout << (*clusterChain)[i] << " ";
    }
    cout << endl;
    */
    string concatLfn = "";
    for (auto& clusterIndex : *clusterChain) {
        unsigned offset = DATA_START + (clusterIndex - 2) * CLUSTER_SIZE;
        fseek(fp, offset, SEEK_SET);
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
                if (fatFile->msdos.filename[0] == 0x2E) {
                    cout << "dot entry filename = -" << fatFile->msdos.filename << "-\n";
                    cout << "dot entry extension = -" << fatFile->msdos.extension << "-\n";
                    cout << "attribute = -" << fatFile->msdos.attributes << "-\n";
                    printf("hex attribute = %X\n", fatFile->msdos.attributes);
                }
                for (int j = 0; j < 8; j++) {
                    if (!isascii(fatFile->msdos.filename[j]) || fatFile->msdos.filename[j] == 0 || fatFile->msdos.filename[j] == 32) {
                        break;
                    }
                    name83.push_back(fatFile->msdos.filename[j]);
                }
                if (concatLfn.size()) { // end of LFN
                    uint32_t firstCluster = (fatFile->msdos.eaIndex << 16) + fatFile->msdos.firstCluster;

                    FileNode* newNode = new FileNode;
                    newNode->name = concatLfn;
                    // cout << "adding new node " << newNode->name << " with first cluster " << firstCluster << endl;
                    newNode->parentRef = root;
                    newNode->isRead = false;
                    newNode->type = attributes == 16 ? _FOLDER : _FILE;
                    newNode->firstClusterIndex = firstCluster;
                    newNode->clusterChain = getClusterChain(firstCluster);
                    newNode->entry = fatFile;
                    string order;
                    for (int i = 1; i < 8; i++) {
                        if (name83[i] == ' ' || name83[i] == 0) {
                            break;
                        }
                        order.push_back(name83[i]);
                    }
                    newNode->order = stoi(order);
                    newNode->setModifiedDate(fatFile->msdos.modifiedDate);
                    newNode->setModifiedTime(fatFile->msdos.modifiedTime);
                    newNode->fileSize = fatFile->msdos.fileSize;
                    newNode->creationMs = fatFile->msdos.creationTimeMs;
                    root->children.push_back(newNode);
                    concatLfn.erase();
                } else {
                    if (name83 == ".") {
                        FileNode* fptr = new FileNode(*root);
                        fptr->name = ".";
                        fptr->realName = root->name;
                        fptr->realNode = root;
                        fptr->type = _DOT;
                        root->children.push_back(fptr);
                    } else if (name83 == "..") {
                        FileNode* fptr = new FileNode(*(root->parentRef));
                        fptr->name = "..";
                        fptr->realName = root->parentRef->name;
                        fptr->realNode = root->parentRef;
                        fptr->type = _DOT;
                        root->children.push_back(fptr);
                    }
                } 
            }
            // delete fatFile;
        }
    }
    fclose(fp);
}

void createTree(FileNode* root) {
    if (root->type == _FOLDER) {
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

void printFatEntries(FileNode* node) {
    FILE* fp = fopen(imgFile, "r");
    for (auto& cluster : *node->clusterChain) {
        cout << "cluster is " << cluster << endl;
        fseek(fp, FAT_START + cluster * 4, SEEK_SET);
        uint8_t* bytes = new uint8_t[4];
        for (int j = 0; j < 4; j++) {
            fread(bytes + j, sizeof(uint8_t), 1, fp);
            printf("0x%X ", bytes[j]);
        }
        delete[] bytes;
        cout << endl;
    }
    fclose(fp);
}

void printCluster(unsigned cluster) {
    FatFileEntry* fatFile = new FatFileEntry;
    FILE* fp = fopen(imgFile, "r");
    fseek(fp, DATA_START + (cluster - 2) * CLUSTER_SIZE, SEEK_SET);
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
                    //break;
                }
                cout << "pushing val" << fatFile->lfn.name1[j] << "i = " << i << endl;
                current.push_back((char) fatFile->lfn.name1[j]);
            }
            for (int j = 0; j < 6; j++) {
                if (!isascii(fatFile->lfn.name2[j]) || fatFile->lfn.name2[j] == 0 || fatFile->lfn.name2[j] == 32) {
                    //break;
                }
                cout << "pushing val" << fatFile->lfn.name2[j] << "i = " << i << endl;
                current.push_back((char) fatFile->lfn.name2[j]);
            }
            for (int j = 0; j < 2; j++) {
                if (!isascii(fatFile->lfn.name3[j]) || fatFile->lfn.name3[j] == 0 || fatFile->lfn.name3[j] == 32) {
                    //break;
                }
                cout << "pushing val" << fatFile->lfn.name3[j] << "i = " << i << endl;
                current.push_back((char) fatFile->lfn.name3[j]);
            }
            concatName = current + concatName;
            cout << "name = " << concatName << "\t\t\t\t\t";
            printf("sequence no = 0x%X ", fatFile->lfn.sequence_number);
            cout << "checksum = " << fatFile->lfn.checksum << endl;
        } else if (attributes == 16 || attributes == 32) { // 8.3 entry
            concatName.erase();
            for (int j = 0; j < 8; j++) {
                if (!isascii(fatFile->msdos.filename[j]) || fatFile->msdos.filename[j] == 0 || fatFile->msdos.filename[j] == 32) {
                    // break;
                }
                if (j == 0) {
                    printf("adding char[0] 0x%X", fatFile->msdos.filename[j]);
                    cout << " int value = " << (int) fatFile->msdos.filename[0] << endl;
                } else {
                    cout << "adding a char -> " << fatFile->msdos.filename[j] << " int value = " << (int) fatFile->msdos.filename[j] << endl;
                }
                name[j] = fatFile->msdos.filename[j];
            }
            cout << "name = ..." << fatFile->msdos.filename << "...\n";
            for (int j = 0; j < 3; j++) {
                cout << "int val of extension + " << j << " = " << (int) fatFile->msdos.extension[j] << endl;
                cout << "attributes = " << fatFile->msdos.attributes << endl;
            }
        }
        printf("file size = %u ", fatFile->msdos.fileSize);
        printf("attributes = 0x%X ", fatFile->msdos.attributes);
        printf("first cluster 0-1 = 0x%X 0x%x ", fatFile->msdos.eaIndex, fatFile->msdos.firstCluster);
        printf("modified date = %u ", fatFile->msdos.modifiedDate);
        printf("modified time = %u ", fatFile->msdos.modifiedTime);
        cout << "order = " << i << endl;
        cout << endl;
        delete[] name;
        delete[] extension;
        delete[] shortName;
    }
    fclose(fp);
    cout << "END OF CLUSTER " << cluster << endl;
}

FileNode* createChild(FileNode* parentDirectory, string name, enum nodeType type) {
    int numLfnEntries = ceil(name.size() / 13.0);
    FileNode* newDirNode = new FileNode;
    newDirNode->name = name;
    newDirNode->order = parentDirectory->getMaxOrder() + 1;
    newDirNode->parentRef = parentDirectory;
    newDirNode->type = _FOLDER;
    newDirNode->clusterChain = new vector<unsigned>;
    vector<unsigned> availableAddresses = getAvailableAddresses(parentDirectory, numLfnEntries + 1);
    bool reserved = reserveNewCluster(newDirNode, 2);
    if ((availableAddresses.size() != numLfnEntries + 1) || !reserved) {
        return nullptr;
    }
    FatFileEntry** entries = new FatFileEntry*[numLfnEntries + 1]; // +1 for 8.3
    FatFileEntry* msdos = new FatFileEntry;
    char checkSumArg[11];
    msdos->msdos.filename[0] = 0x7E;
    checkSumArg[0] = 0x7E;
    string order = to_string(parentDirectory->getMaxOrder() + 1);
    int copiedChars = 0;
    while (copiedChars < order.size()) {
        msdos->msdos.filename[copiedChars + 1] = order[copiedChars];
        checkSumArg[copiedChars + 1] = order[copiedChars];
        copiedChars++;
    }
    for (int i = copiedChars + 1; i < 8; i++) {
        msdos->msdos.filename[i] = 0x20;
        checkSumArg[i] = 0x20;
    }
    
    for (int i = 0; i < 3; i++) {
        msdos->msdos.extension[i] = 0x20;
        checkSumArg[i + 8] = 0x20;
    }
    msdos->msdos.fileSize = 0;
    msdos->msdos.eaIndex = (newDirNode->firstClusterIndex & 0xFFFF0000) >> 16;
    msdos->msdos.firstCluster = newDirNode->firstClusterIndex & 0x0000FFFF;
    msdos->msdos.attributes = 0x10;
    msdos->msdos.reserved = 0;

    uint16_t creationTime = getCurrentTime();
    uint16_t creationDate = getCurrentDate();
    msdos->msdos.creationTimeMs = getCurrentMs();
    msdos->msdos.creationTime = creationTime;
    msdos->msdos.modifiedTime = creationTime;
    msdos->msdos.creationDate = creationDate;
    msdos->msdos.modifiedDate = creationDate;
    newDirNode->setModifiedDate(creationDate);
    newDirNode->setModifiedTime(creationTime);
    newDirNode->entry = msdos;
    entries[numLfnEntries] = msdos;
    uint8_t checksum = lfn_checksum(checkSumArg);
    vector<uint16_t> padding;
    padding.reserve(13 * numLfnEntries);
    padding[name.size()] = 0x00;
    for (int i = name.size() + 1; i < 13 * numLfnEntries; i++) {
        padding[i] = 0xFF;
    }
    for (int k = 0; k < numLfnEntries; k++) {
        FatFileEntry* lfn = new FatFileEntry;             
        lfn->lfn.reserved = 0x00;
        lfn->lfn.attributes = 0x0F;
        lfn->lfn.checksum = checksum;

        lfn->lfn.firstCluster = 0x0000;
        if (k == numLfnEntries - 1) {
            lfn->lfn.sequence_number = 0x40 + numLfnEntries;
        } else {
            lfn->lfn.sequence_number = k + 1;
        }
        for (int j = 0; j < 5; j++) {
            lfn->lfn.name1[j] = name.size() > k * 13 + j ? name[k * 13 + j] : padding[k * 13 + j];
        }
        for (int j = 5; j < 11; j++) {
            lfn->lfn.name2[j - 5] = name.size() > k * 13 + j ? name[k * 13 + j] : padding[k * 13 + j];
        }
        for (int j = 11; j < 13; j++) {
            lfn->lfn.name3[j - 11] = name.size() > k * 13 + j ? name[k * 13 + j] : padding[k * 13 + j];
        }
        entries[numLfnEntries - 1 - k] = lfn;
    }
    FILE* fpx = fopen(imgFile, "r+");
    for (int i = 0; i < availableAddresses.size(); i++) {
        fseek(fpx, availableAddresses[i], SEEK_SET);
        fwrite(entries[i], sizeof(FatFileEntry), 1, fpx);
    }
    fclose(fpx);
    parentDirectory->children.push_back(newDirNode);
    return newDirNode;
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
    FatFileLFN* fatLFN = new FatFileLFN;
    void* vp = ((void*) bpb) + 36;
    bpb32 = (BPB32_struct*) vp;
    FILE* fp2 = fopen(argv[1], "r");
    fread(bpb, sizeof(BPB32_struct), 1, fp2);
    cout << "root cluster = " << bpb32->RootCluster << endl;

    // fread(&EOCVAL, 4, 1, fp2);
    /*
    uint8_t** fatEntry = new uint8_t*[100];
    for (int i = 0; i < 100; i++) {
        fatEntry[i] = new uint8_t[4];
        //printf("0x%X: ", i);
        for (int j = 0; j < 4; j++) {
            fread(&fatEntry[i][j], sizeof(uint8_t), 1, fp2);
            //printf("0x%X ", fatEntry[i][j]);
        }
        if (i == 1) {
            // TODO: WILL BE REMOVED FROM HERE. ASSIGN EOCVAL IN UPPER SIDE
            EOCVAL = (fatEntry[1][0]) + (fatEntry[1][1] << 8) + (fatEntry[1][2] << 16) + (fatEntry[1][3] << 24);
            break;
        }
        // cout << endl;
    }
    */
    fclose(fp2);
    // cout << endl;    
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
    root->firstClusterIndex = bpb32->RootCluster;
    cout << "root first cluster is " << root->firstClusterIndex << endl;
    root->clusterChain = getClusterChain(root->firstClusterIndex); // will have memleak;
    root->type = _FOLDER;
    root->isRead = false;
    const clock_t begin_time = clock();
    createTree(root);
    cout << "CLOCK" << float( clock () - begin_time ) /  CLOCKS_PER_SEC << endl;
    *fileTree = root;
    string line;
    FileNode* currentDir = root;
    while (1) {
        cout << pwd << "> ";
        getline(cin, line);
        vector<string> command = tokenizeString(line, ' ');
        if (!command.size()) { continue; }
        if (command[0] == "quit") {
            break;
        } else if (command[0] == "cd") {
            string arg1 = string(command[1]);
            vector<string> directories = extractDirectories(arg1);
            FileNode* directory = findDirectory(currentDir, directories);
            /*
            if (command[1] != "/") { // update last access time
                FILE* fp1 = fopen(imgFile, "r+");
                time_t now = time(0);
                tm *tm = localtime(&now);
                uint16_t creationTime = (tm->tm_hour << 11) + (tm->tm_min << 5) + tm->tm_sec / 2;
                for (auto& cluster : *directory->parentRef->clusterChain) {
                    FatFileEntry* fe = new FatFileEntry;
                    fseek(fp1, DATA_START + cluster * CLUSTER_SIZE + directory->order * 32, SEEK_SET);
                    fread(fe, sizeof(FatFileEntry), 1, fp1);
                    fe->msdos.l
                }
                fclose(fp1);
            }
            */
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
                for (auto& child : listedDirectory->children) {
                    if (child->type == _FOLDER || child->type == _DOT) {
                        cout << "drwx------ 1 root root 0 ";
                    } else {
                        cout << "-rwx------ 1 root root " << child->fileSize << " ";
                    }
                    cout << child->modifiedYear << " " << child->modifiedMonth << " " << child->modifiedDay
                    << " ";
                    if (child->modifiedHour < 10) {
                        cout << "0";
                    }
                    cout << child->modifiedHour << ":";
                    if (child->modifiedMinute < 10) {
                        cout << "0";
                    }
                    cout << child->modifiedMinute << " " << child->name << endl;
                }
            } else { // ls <path> or ls
                if (command.size() > 1) {
                    vector<string> directories = extractDirectories(command[1]);
                    FileNode* directory = findDirectory(currentDir, directories);
                    if (directory == nullptr) {
                        continue;
                    }
                    listedDirectory = directory;
                    if (directory->name == "folder2") {
                        cout << directory->children.size() << " = size" << endl;
                    }
                }
                for (auto& child : listedDirectory->children) {
                    cout << child->name << " ";
                }
                cout << endl;
            }
        } else if (command[0] == "mkdir") {
            vector<string> directories = extractDirectories(command[1]);
            string folderName = directories[directories.size() - 1];
            FileNode* parentDirectory;
            bool parentContainsFolder = false;
            int numLfnEntries = ceil(folderName.size() / 13.0);
            if (directories.size() > 1) {
                vector<string> v(directories.begin(), directories.end() - 1);
                parentDirectory = findDirectory(currentDir, v);
            } else {
                parentDirectory = currentDir;
            }
            if (parentDirectory == nullptr) {
                continue;
            }
            for (auto& child : parentDirectory->children) {
                if (child->name == folderName) {
                    parentContainsFolder = true;
                    break;
                }
            }
            if (parentContainsFolder) {
                continue;
            }
            FileNode* newDirNode = createChild(parentDirectory, folderName, _FOLDER);
            if (newDirNode == nullptr) {
                continue;
            }
            
            // CREATE . and .. 
            FileNode* dotEntry = new FileNode(*newDirNode);
            dotEntry->name = ".";
            dotEntry->realName = newDirNode->name;
            dotEntry->realNode = newDirNode;
            dotEntry->type = _DOT;
            newDirNode->children.push_back(dotEntry);
            FatFileEntry* dot83 = new FatFileEntry;
            dot83->msdos.filename[0] = 0x2E;
            for (int j = 1; j < 8; j++) {
                dot83->msdos.filename[j] = 0x20;
            }
            for (int j = 0; j < 3; j++) {
                dot83->msdos.extension[j] = 0x20;
            }
            dot83->msdos.fileSize = 0;
            dot83->msdos.attributes = 0x10;
            dot83->msdos.reserved = 0;
            uint16_t creationTime = getCurrentTime();
            uint16_t creationDate = getCurrentDate();
            dot83->msdos.creationTimeMs = getCurrentMs();
            dot83->msdos.creationDate = creationDate;
            dot83->msdos.creationTime = creationTime;
            dot83->msdos.modifiedDate = creationDate;
            dot83->msdos.modifiedTime = creationTime;
            dot83->msdos.eaIndex = (newDirNode->firstClusterIndex & 0xFFFF0000) >> 16;
            dot83->msdos.firstCluster = (newDirNode->firstClusterIndex & 0x0000FFFF);

            FileNode* twoDotEntry = new FileNode(*parentDirectory);
            twoDotEntry->name = "..";
            twoDotEntry->realName = parentDirectory->name;
            twoDotEntry->realNode = parentDirectory;
            twoDotEntry->type = _DOT;
            newDirNode->children.push_back(twoDotEntry);
            FatFileEntry* twoDot83 = new FatFileEntry;
            twoDot83->msdos.filename[0] = 0x2E;
            twoDot83->msdos.filename[1] = 0x2E;
            for (int j = 2; j < 8; j++) {
                twoDot83->msdos.filename[j] = 0x20;
            }
            for (int j = 0; j < 3; j++) {
                twoDot83->msdos.extension[j] = 0x20;
            }
            twoDot83->msdos.fileSize = 0;
            twoDot83->msdos.attributes = 0x10;
            twoDot83->msdos.reserved = 0;
            twoDot83->msdos.creationTimeMs = getCurrentMs();
            twoDot83->msdos.creationDate = creationDate;
            twoDot83->msdos.creationTime = creationTime;
            twoDot83->msdos.modifiedDate = creationDate;
            twoDot83->msdos.modifiedTime = creationTime;
            twoDot83->msdos.eaIndex = parentDirectory->name == "/" ? 0 : (parentDirectory->firstClusterIndex & 0xFFFF0000) >> 16;
            twoDot83->msdos.firstCluster = parentDirectory->name == "/" ? 0 : (parentDirectory->firstClusterIndex & 0x0000FFFF);
            FILE* fpx = fopen(imgFile, "r+");
            fseek(fpx, DATA_START + CLUSTER_SIZE * (newDirNode->firstClusterIndex - 2), SEEK_SET);
            fwrite(dot83, sizeof(FatFileEntry), 1, fpx);
            fseek(fpx, DATA_START + CLUSTER_SIZE * (newDirNode->firstClusterIndex - 2) + 32, SEEK_SET);
            fwrite(twoDot83, sizeof(FatFileEntry), 1, fpx);
            fclose(fpx);
            // TODO: Correct free cluster summary in FS
        } else if (command[0] == "touch") {

        } else if (command[0] == "test") {
            cout << "root cluster chain is: " << endl;
            for (auto& idx : *((*fileTree)->clusterChain)) {
                cout << idx << endl;
            }
            cout << "folder 1 cluster chain is: " << endl;
            for (auto& idx : *(fileTree[0]->children[0]->clusterChain)) {
                cout << idx << endl;
            }
            cout << "printing root clusters " << endl;
            for (auto& idx : *((*fileTree)->clusterChain)) {
                printCluster(idx);
            }
            cout << "printing data cluster 591 " << endl;
            printCluster(591);
            cout << "printing data cluster 1 "<< endl;
            printCluster(1);
        } else if (command[0] == "checksumtest") {
            char testsum[11];
            testsum[0] = fileTree[0]->children[0]->entry->msdos.filename[0];
            cout << "00 name = " << fileTree[0]->children[0]->name << endl;
            printf("00 attributes = 0x%X", fileTree[0]->children[0]->entry->msdos.attributes);
            printf("testsum0 = 0x%X\n", testsum[0]);
            testsum[1] = fileTree[0]->children[0]->entry->msdos.filename[1];
            for (int k = 2; k < 11; k++) {
                testsum[k] = ' ';
            }
            uint8_t cs = lfn_checksum(testsum);

            cout << "checksum of [0][0] is = " << cs << endl;
        } else if (command[0] == "printc") {
            printCluster(stoi(command[1]));
        } else if (command[0] == "printcc") {
            printFatEntries(currentDir);
        }

    }

    
    return 0;
}
