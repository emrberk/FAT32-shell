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

using namespace std;

vector<string> MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

enum nodeType {_FILE, _FOLDER, _DOT};

unsigned EOCVAL;
unsigned FAT_START;
unsigned FAT_SIZE;
unsigned DATA_START;
unsigned FS_INFO_START;
unsigned CLUSTER_SIZE;
unsigned NUM_FATS;
unsigned FREE_CLUSTERS;
FatFileEntry* ZERO_ENTRY;
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

uint32_t toLittleEndian(uint32_t number) {
    return (number & 0xFF000000) >> 24 + (number & 0x00FF0000) >> 16 + (number & 0x0000FF00) >> 8 + (number & 0x000000FF);
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
    enum nodeType type;             
    vector<unsigned>* clusterChain;
    FileNode* parentRef;
    unsigned firstClusterIndex;
    vector<FileNode*> children;
    FatFileEntry* entry;
    int order;
    uint8_t checksum;
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
        checksum = 0;
        clusterChain = nullptr;
        parentRef = nullptr;
        firstClusterIndex = 0;
        fileSize = 0;
        entry = nullptr;
        realNode = nullptr;
        order = 0;
    }
    FileNode(string name, enum nodeType type, vector<unsigned>* clusterChain, FileNode* parentRef, unsigned firstClusterIndex, vector<FileNode*> children, FatFileEntry* entry) :
        name(name),
        type(type),
        clusterChain(clusterChain),
        parentRef(parentRef),
        children(children),
        entry(entry),
        firstClusterIndex(firstClusterIndex)
    {}
    FileNode(const FileNode& base) {
        order = 0;
        fileSize = base.fileSize;
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

    bool isListable() {
        return (this->name != "/" &&  this->children.size() > 2) || (this->name == "/" && this->children.size() > 0);
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

bool isChild(FileNode* first, FileNode* second) {
    bool cond = false;
    for (auto& child : second->children) {
        if (child->type != _DOT) {
            cond = (child == first) || isChild(first, child);
        }
    }
    return cond;
}

void updateFAT(FileNode* parentDirectory, deque<unsigned> newClusterIndices) {
    FILE* fp = fopen(imgFile, "r+");
    unsigned currentFATStart = FAT_START;
    int i = 0;
    vector<unsigned> parentChain = (*parentDirectory->clusterChain);
    while (i < NUM_FATS) {
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
        currentFATStart += FAT_SIZE;
        i++;
    }
    fclose(fp);
}

void updateTimes(FileNode* parentDirectory, uint16_t date, uint16_t time) {
    parentDirectory->setModifiedDate(date);
    parentDirectory->setModifiedTime(time);
    bool foundLfn = false;
    bool completed = false;
    FileNode* grandFather = parentDirectory->parentRef;
    FILE* fpx = fopen(imgFile, "r+");
    for (auto& cluster : *(grandFather->clusterChain)) {
        for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry) && !completed; i++) {
            unsigned long offset = DATA_START + (cluster - 2) * CLUSTER_SIZE + i * sizeof(FatFileEntry);
            fseek(fpx, offset, SEEK_SET);
            FatFileEntry* entry = new FatFileEntry;
            fread(entry, sizeof(FatFileEntry), 1, fpx);
            if (entry->msdos.attributes == 0xF && entry->lfn.checksum == parentDirectory->checksum) {
                foundLfn = true;
            } else if (foundLfn && entry->msdos.attributes == 0x10) {
                entry->msdos.modifiedDate = date;
                entry->msdos.modifiedTime = time;
                fseek(fpx, offset, SEEK_SET);
                fwrite(entry, sizeof(FatFileEntry), 1, fpx);
                completed = true;
            }
            delete entry;
        }
        if (completed) {
            break;
        }
    }
}
/*
bool updateParent(FileNode* file, int emptyClusterIndex) {
    FILE* fp = fopen(imgFile, "r+");
    unsigned numEntries = ceil(file->name.size() / 13.0) + 1;
    if (emptyClusterIndex == 0) {
        for (auto& parentCluster : *(file->parentRef->clusterChain)) {
            for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry); i++) {
                FatFileEntry* buffer;
                unsigned offset = DATA_START + (parentCluster - 2) * CLUSTER_SIZE + i * sizeof(FatFileEntry);
                fseek(fp, offset, SEEK_SET);
                fread(buffer, sizeof(FatFileEntry), 1, fp);
                if (buffer->lfn.checksum == file->checksum) {
                    numEntries--;
                } else if (numEntries == 0 && emptyClusterIndex == 0) {
                    // NECESSARY? buffer->msdos.lastAccessTime
                    unsigned newStart = file->parentRef->clusterChain->at(1);
                    buffer->msdos.eaIndex = (newStart & 0xFFFF0000) >> 16;
                    buffer->msdos.firstCluster = (newStart & 0x0000FFFF);
                    fseek(fp, offset, SEEK_SET);
                    fwrite(buffer, sizeof(FatFileEntry), 1, fp);
                }
            }
        }
    }
    if (emptyClusterIndex == -1) {
        return true;
    }
    uint32_t zeroEntry = 0;
    for (int i = 0; i < NUM_FATS; i++) {
        fseek(fp, FAT_START + i * FAT_SIZE + file->clusterChain->at(emptyClusterIndex) * 4, SEEK_SET);
        fwrite(&zeroEntry, 4, 1, fp);
    }
    if (emptyClusterIndex == 0) {
        return true;
    }
    size_t parentCCSize = file->parentRef->clusterChain->size();
    if (parentCCSize == 2) {
        file->parentRef->clusterChain->erase(file->parentRef->clusterChain->begin() + emptyClusterIndex);
        return true;
    }
    deque<unsigned> after;
    for (int i = emptyClusterIndex + 1; i < parentCCSize; i++) {
        after.push_back(file->parentRef->clusterChain->at(i));
    }
    for (int i = emptyClusterIndex; i < parentCCSize; i++) {
        file->parentRef->clusterChain->erase(file->parentRef->clusterChain->begin() + i);
    }
    fclose(fp);
    updateFAT(file->parentRef, after);
    return true;
}
*/

bool reserveNewCluster(FileNode* parentDirectory, unsigned remainingEntries) {
    unsigned neededClusters = remainingEntries / (CLUSTER_SIZE / sizeof(FatFileEntry)) + 1;
    FILE* fp = fopen(imgFile, "r");
    deque<unsigned> newClusterIndices;
    for (unsigned i = 2; i < FAT_SIZE / 4, newClusterIndices.size() < neededClusters; i++) {
        fseek(fp, FAT_START + i * 4, SEEK_SET);
        uint32_t entry;
        fread(&entry, 4, 1, fp);
        if (entry != 0) {
            continue;
        }
        fseek(fp, DATA_START + (i - 2) * CLUSTER_SIZE, SEEK_SET);
        bool fullEmpty = true;
        for (unsigned j = 0; j < CLUSTER_SIZE / sizeof(FatFileEntry); j++) {
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
        FILE* fp2 = fopen(imgFile, "r+");
        FREE_CLUSTERS -= neededClusters;
        fseek(fp2, FS_INFO_START + 488, SEEK_SET);
        fwrite(&FREE_CLUSTERS, 4, 1, fp2);
        fclose(fp2);
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
        for (unsigned i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry); i++) {
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
    if (currentCluster == 0) { // For empty files
        return clusterChain;
    }
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
    FILE* fp = fopen(imgFile, "r+");
    vector<unsigned>* clusterChain = root->clusterChain;
    string concatLfn = "";
    uint8_t checksum = 0;
    for (auto& clusterIndex : *clusterChain) {
        unsigned offset = DATA_START + (clusterIndex - 2) * CLUSTER_SIZE;
        fseek(fp, offset, SEEK_SET);
        for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry); i++) {
            FatFileEntry* fatFile = new FatFileEntry; // memleak
            string name83;
            fread(fatFile, sizeof(FatFileEntry), 1, fp);
            unsigned attributes = fatFile->msdos.attributes;
            if (fatFile->msdos.filename[0] == 0xE5) { // Deleted entry fix
                fseek(fp, offset + i * sizeof(FatFileEntry), SEEK_SET);
                fwrite(ZERO_ENTRY, sizeof(FatFileEntry), 1, fp);
            } else if (attributes == 0xF) { // LFN entry
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
                checksum = fatFile->lfn.checksum;
            } else if (attributes == 0x10 || attributes == 0x20) { // 8.3 entry
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
                    newNode->parentRef = root;
                    newNode->type = attributes == 16 ? _FOLDER : _FILE;
                    newNode->firstClusterIndex = firstCluster;
                    newNode->clusterChain = getClusterChain(firstCluster);
                    newNode->entry = fatFile;
                    newNode->checksum = checksum;
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
                    checksum = 0;
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
            } // else if (attributes == )
            delete fatFile;
        }
    }
    fclose(fp);
}

void createTree(FileNode* root) {
    if (root->type == _FOLDER) {
        getFileAndFolders(root);
        for (auto& child : root->children) {
            createTree(child);
        }
    }
}

FileNode* findFile(FileNode* currentDir, vector<string>& directories) {
    bool found = false;
    if (directories.size() == 0) {
        return nullptr;
    }

    if (directories[0] == "/") {
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
                return findFile(child, newVector);
            } else if (child->type == _DOT) {
                if (directories.size() == 1) {
                    return child->realNode;
                }
                vector<string> newVector(directories.begin() + 1, directories.end());
                return findFile(child->realNode, newVector);
            } else if (child->type == _FILE && directories.size() == 1) {
                return child;
            }
        } else if (directories[0] == "." && currentDir->name == "/") {
            if (directories.size() == 1) {
                return currentDir;
            }
            vector<string> newVector(directories.begin() + 1, directories.end());
            return findFile(currentDir, newVector);
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
    for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry); i++) { // ROOT DIRECTORY - CLUSTER 2
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
                    break;
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

bool createDotEntries(FileNode* newDirNode) {
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
    FileNode* parentDirectory = newDirNode->parentRef;
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
}

FileNode* searchForParent(FileNode* currentDir, vector<string> directories) {
    string folderName = directories[directories.size() - 1];
    FileNode* parentDirectory;
    bool parentContainsFolder = false;
    if (directories.size() > 1) {
        vector<string> v(directories.begin(), directories.end() - 1);
        parentDirectory = findFile(currentDir, v);
    } else {
        parentDirectory = currentDir;
    }
    if (parentDirectory == nullptr) {
        return nullptr;
    }
    for (auto& child : parentDirectory->children) {
        if (child->name == folderName) {
            parentContainsFolder = true;
            break;
        }
    }
    if (parentContainsFolder) {
        return nullptr;
    }
    return parentDirectory;
}

FileNode* createChild(FileNode* parentDirectory, string name, enum nodeType type) {
    int numLfnEntries = ceil(name.size() / 13.0);
    FileNode* newDirNode = new FileNode;
    newDirNode->name = name;
    newDirNode->order = parentDirectory->getMaxOrder() + 1;
    newDirNode->parentRef = parentDirectory;
    newDirNode->type = type;
    newDirNode->clusterChain = new vector<unsigned>;
    vector<unsigned> availableAddresses = getAvailableAddresses(parentDirectory, numLfnEntries + 1);
    if (availableAddresses.size() != numLfnEntries + 1) {
        return nullptr;
    }
    if (type == _FOLDER && reserveNewCluster(newDirNode, 2) == false) {
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
    msdos->msdos.attributes = type == _FOLDER ? 0x10 : 0x20;
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
    newDirNode->checksum = checksum;
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
    if (parentDirectory->name != "/") {
        updateTimes(parentDirectory, creationDate, creationTime);   
    }
    parentDirectory->children.push_back(newDirNode);
    if (type == _FOLDER) {
        bool created = createDotEntries(newDirNode);
        if (!created) return nullptr;
    }
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
    ZERO_ENTRY = (FatFileEntry*) calloc(1, sizeof(FatFileEntry));
    imgFile = argv[1];
    BPB_struct* bpb = new BPB_struct;
    BPB32_struct* bpb32 = new BPB32_struct;
    void* vp = ((void*) bpb) + 36;
    bpb32 = (BPB32_struct*) vp;
    FILE* fp2 = fopen(argv[1], "r");
    fread(bpb, sizeof(BPB_struct), 1, fp2);
    CLUSTER_SIZE = bpb->BytesPerSector * bpb->SectorsPerCluster;
    FAT_START = bpb->ReservedSectorCount * bpb->BytesPerSector;
    FAT_SIZE = bpb32->FATSize * bpb->BytesPerSector;
    NUM_FATS = bpb->NumFATs;
    DATA_START = FAT_START + NUM_FATS * FAT_SIZE;
    FS_INFO_START = bpb->BytesPerSector * bpb32->FSInfo;
    fseek(fp2, FAT_START, SEEK_SET);
    fread(&EOCVAL, 4, 1, fp2);
    fseek(fp2, FS_INFO_START + 488, SEEK_SET);
    fread(&FREE_CLUSTERS, 4, 1, fp2);
    fclose(fp2);
    string pwd = "/";
    FileNode* root = new FileNode;
    root->name = "/";
    root->firstClusterIndex = bpb32->RootCluster;
    root->clusterChain = getClusterChain(root->firstClusterIndex); // will have memleak;
    root->type = _FOLDER;
    const clock_t begin_time = clock();
    createTree(root);
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
            FileNode* directory = findFile(currentDir, directories);
            if (directory != nullptr && directory->type == _FOLDER) {
                pwd = findAbsolutePath(directory);
                currentDir = directory;
            }

        } else if (command[0] == "ls") {
            FileNode* listedDirectory = currentDir;
            if (command.size() > 1 && command[1] == "-l") {
                if (command.size() > 2) { // ls -l <path>
                    vector<string> directories = extractDirectories(command[2]);
                    FileNode* directory = findFile(currentDir, directories);
                    listedDirectory = directory;
                }
                if (listedDirectory == nullptr || !listedDirectory->isListable()) {
                    continue;
                }
                for (auto& child : listedDirectory->children) {
                    if (child->type == _DOT) {
                        continue;
                    } else if (child->type == _FOLDER) {
                        cout << "drwx------ 1 root root 0 ";
                    } else if (child->type == _FILE) {
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
                    FileNode* directory = findFile(currentDir, directories);
                    listedDirectory = directory;
                }
                if (listedDirectory == nullptr || !listedDirectory->isListable()) {
                    continue;
                }
                for (auto& child : listedDirectory->children) {
                    if (child->type != _DOT) {
                        cout << child->name << " ";
                    }
                }
                cout << endl;
            }
        } else if (command[0] == "mkdir") {
            vector<string> directories = extractDirectories(command[1]);
            string folderName = directories[directories.size() - 1];
            FileNode* parentDirectory = searchForParent(currentDir, directories);
            if (parentDirectory == nullptr) {
                continue;
            }
            FileNode* newDirNode = createChild(parentDirectory, folderName, _FOLDER);
            // TODO: Better error check
            if (newDirNode == nullptr) {
                continue;
            }
            // TODO: Correct free cluster summary in FS
        } else if (command[0] == "touch") {
            vector<string> directories = extractDirectories(command[1]);
            string fileName = directories[directories.size() - 1];
            FileNode* parentDirectory = searchForParent(currentDir, directories);
            if (parentDirectory == nullptr) {
                continue;
            }
            FileNode* newFileNode = createChild(parentDirectory, fileName, _FILE);
            // TODO: Better error check
            if (newFileNode == nullptr) {
                continue;
            }
        } else if (command[0] == "cat") {
            vector<string> directories = extractDirectories(command[1]);
            FileNode* file = findFile(currentDir, directories);
            if (file == nullptr || file->type != _FILE) {
                continue;
            }
            FILE* fp = fopen(imgFile, "r");
            for (auto& cluster : *(file->clusterChain)) {
                fseek(fp, DATA_START + (cluster - 2) * CLUSTER_SIZE, SEEK_SET);
                char* c = new char;
                for (int i = 0; i < CLUSTER_SIZE; i++) {
                    fread(c, sizeof(char), 1, fp);
                    cout << *c;
                }
                delete c;
            }
            fclose(fp);
        } else if (command[0] == "mv") {
            // Find source & destination
            vector<string> sourceDirectories = extractDirectories(command[1]);
            vector<string> destinationDirectories = extractDirectories(command[2]);
            FileNode* source = findFile(currentDir, sourceDirectories);
            if (source == nullptr || source->type == _DOT || source->name == "/") {
                continue;
            }
            FileNode* srcParent = source->parentRef;
            FileNode* destinationFolder = findFile(currentDir, destinationDirectories);
            if (destinationFolder == nullptr || srcParent == destinationFolder || isChild(destinationFolder, source)) {
                continue;
            }
            bool parentContains = false;
            for (auto& child: destinationFolder->children) {
                if (child->name == source->name) {
                    parentContains = true;
                    break;
                }
            }
            if (parentContains) {
                continue;
            }
            // Take FatFileEntries from source parent directory and set spaces to ZERO_ENTRY
            unsigned numEntries = ceil(source->name.size() / 13.0) + 1;
            vector<FatFileEntry> sourceEntries;
            FILE* fp = fopen(imgFile, "r+");
            bool insideSourceEntries = false;
            bool completed = false;
            int j = 0;
            unsigned lastCluster = -1;
            int lastClusterIndex = -1;
            for (auto& cluster : *(srcParent->clusterChain)) {
                for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry) && !completed; i++) {
                    unsigned long offset = DATA_START + (cluster - 2) * CLUSTER_SIZE + i * sizeof(FatFileEntry);
                    FatFileEntry* buffer = new FatFileEntry;
                    fseek(fp, offset, SEEK_SET);
                    fread(buffer, sizeof(FatFileEntry), 1, fp);
                    if (insideSourceEntries) {
                        sourceEntries.push_back(*buffer);
                        fseek(fp, offset, SEEK_SET);
                        fwrite(ZERO_ENTRY, sizeof(FatFileEntry), 1, fp);
                        if (sourceEntries.size() == numEntries) {
                            lastClusterIndex = j;
                            lastCluster = cluster;
                            completed = true;
                        }
                    } else if (buffer->msdos.attributes != 0xE5 && buffer->lfn.checksum == source->checksum) {
                        sourceEntries.push_back(*buffer);
                        fseek(fp, offset, SEEK_SET);
                        fwrite(ZERO_ENTRY, sizeof(FatFileEntry), 1, fp);
                        insideSourceEntries = true;
                    }
                    delete buffer;
                }
                if (completed) {
                    break;
                }
                j++;
            }
            if (lastCluster == -1) {
                continue;
            }
            /*
             If parent cluster became empty, deallocate it 
            bool fullEmpty = true;
            for (int i = 0; i < CLUSTER_SIZE / sizeof(FatFileEntry); i++) {
                FatFileEntry* buffer = new FatFileEntry;
                unsigned long offset = DATA_START + (lastCluster - 2) * CLUSTER_SIZE + i * sizeof(FatFileEntry);
                fseek(fp, offset, SEEK_SET);
                fread(buffer, sizeof(FatFileEntry), 1, fp);
                if (buffer->msdos.attributes != 0) {
                    fullEmpty = false;
                    break;
                }
            }
            if (fullEmpty && srcParent->clusterChain->size() > 1 && srcParent->name != "/") { // full empty and can be shrinked
                updateParent(srcParent, lastClusterIndex);
            }
            */
            uint16_t currDate = getCurrentDate();
            uint16_t currTime = getCurrentTime();
            fclose(fp);
            if (srcParent->name != "/") {
                updateTimes(srcParent, currDate, currTime);
            }
            // Update source parent directory FileNode
            for (int i = 0; i < srcParent->children.size(); i++) {
                if (srcParent->children[i]->name == source->name) {
                    srcParent->children.erase(srcParent->children.begin() + i);
                    break;
                }
            }
            // Update source FatFileEntries

            // Update source/..
            fp = fopen(imgFile, "r+");
            FatFileEntry* twoDot = new FatFileEntry;
            unsigned firstCluster = source->clusterChain->at(0);
            unsigned long offset = DATA_START + (firstCluster - 2) * CLUSTER_SIZE + sizeof(FatFileEntry);
            fseek(fp, offset, SEEK_SET);
            fread(twoDot, sizeof(FatFileEntry), 1, fp);
            unsigned parentFirstCluster = destinationFolder->clusterChain->at(0);
            twoDot->msdos.eaIndex = (parentFirstCluster & 0xFFFF0000) >> 16;
            twoDot->msdos.firstCluster = (parentFirstCluster & 0x0000FFFF);
            fseek(fp, offset, SEEK_SET);
            fwrite(twoDot, sizeof(FatFileEntry), 1, fp);
            delete twoDot;
            fclose(fp);
            // Update source FileNode
            source->parentRef = destinationFolder;
            source->order = destinationFolder->getMaxOrder() + 1;
            if (source->type == _FOLDER) {
                for (auto& child : source->children) {
                    if (child->name == "..") {
                        child->realNode = destinationFolder;
                        child->realName = destinationFolder->name;
                        break;
                    }
                }
            }
            uint8_t* new83Name = new uint8_t[8];
            new83Name[0] = 0x7E;
            string orderStr = to_string(source->order);
            for (int i = 0; i < orderStr.size(); i++) {
                new83Name[i + 1] = orderStr[i];
            }
            for (int i = orderStr.size() + 1; i < 8; i++) {
                new83Name[i] = ' ';
            }
            for (int i = 0; i < 8; i++) {
                sourceEntries[sourceEntries.size() - 1].msdos.filename[i] = new83Name[i];
            }
            char* new83Char = new char[11];
            for (int i = 0; i < 8; i++) {
                new83Char[i] = new83Name[i];
            }
            for (int i = 0; i < 3; i++) {
                new83Char[i + 8] = 0x20; 
            }
            uint8_t checksum = lfn_checksum(new83Char);
            for (int i = 0; i < sourceEntries.size() - 1; i++) {
                sourceEntries[i].lfn.checksum = checksum;
            }
            delete[] new83Char;
            // Place FatFileEntries under destination
            vector<unsigned> addresses = getAvailableAddresses(destinationFolder, numEntries);
            fp = fopen(imgFile, "r+");
            for (int i = 0; i < addresses.size(); i++) {
                fseek(fp, addresses[i], SEEK_SET);
                fwrite(&(sourceEntries[i]), sizeof(FatFileEntry), 1, fp);
            }
            fclose(fp);
            if (destinationFolder->name != "/") {
                updateTimes(destinationFolder, currDate, currTime);
            }
            // Update destination parent directory FileNode
            destinationFolder->children.push_back(source);

        } 
        
        else if (command[0] == "checksumtest") {
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
