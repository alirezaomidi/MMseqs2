//
// Created by Martin Steinegger on 2019-01-04.
//
#include "FileUtil.h"
#include "LinsearchIndexReader.h"
#include "PrefilteringIndexReader.h"
#include "Debug.h"
#include "Timer.h"
#include "NucleotideMatrix.h"
#include "SubstitutionMatrix.h"
#include "ReducedMatrix.h"
#include "KmerIndex.h"
#include "kmersearch.h"
#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif


template <int TYPE>
size_t LinsearchIndexReader::pickCenterKmer(KmerPosition *hashSeqPair, size_t splitKmerCount) {
    size_t writePos = 0;
    size_t prevHash = hashSeqPair[0].kmer;
    if (TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
        prevHash = BIT_SET(prevHash, 63);
    }
    size_t prevHashStart = 0;
    size_t prevSetSize = 0;
    for (size_t elementIdx = 0; elementIdx < splitKmerCount + 1; elementIdx++) {
        size_t currKmer = hashSeqPair[elementIdx].kmer;
        if (TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
            currKmer = BIT_SET(currKmer, 63);
        }
        if (prevHash != currKmer) {
            size_t difference = (elementIdx-prevHashStart);
            int randomModulo = std::min(static_cast<int>(difference), 10);
            size_t indexToPick = 0;
            //static_cast<size_t >(rand() % randomModulo)
            size_t randIdx = prevHashStart + indexToPick;
            size_t kmer = hashSeqPair[randIdx].kmer;
            if (TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
                kmer = BIT_SET(hashSeqPair[randIdx].kmer, 63);
            }
            // remove singletones from set
            if (kmer != SIZE_T_MAX) {
                hashSeqPair[writePos].kmer = hashSeqPair[randIdx].kmer;
                hashSeqPair[writePos].pos = hashSeqPair[randIdx].pos;
                hashSeqPair[writePos].seqLen = hashSeqPair[randIdx].seqLen;
                hashSeqPair[writePos].id = hashSeqPair[randIdx].id;
                writePos++;
            }
            prevHashStart = elementIdx;
        }

        if (hashSeqPair[elementIdx].kmer == SIZE_T_MAX) {
            break;
        }
        prevSetSize++;
        prevHash = hashSeqPair[elementIdx].kmer;
        if (TYPE == Parameters::DBTYPE_NUCLEOTIDES) {
            prevHash = BIT_SET(prevHash, 63);
        }
    }
    hashSeqPair[writePos].kmer = SIZE_T_MAX;
    return writePos;
}

template size_t LinsearchIndexReader::pickCenterKmer<0>(KmerPosition *hashSeqPair, size_t splitKmerCount);
template size_t LinsearchIndexReader::pickCenterKmer<1>(KmerPosition *hashSeqPair, size_t splitKmerCount);

template <int TYPE, class Comp>
void LinsearchIndexReader::mergeAndWriteIndex(DBWriter & dbw, std::vector<std::string> tmpFiles, int alphSize, int kmerSize) {
    KmerIndex kmerIndex(alphSize, kmerSize);
    Debug(Debug::INFO) << "Merge splits ... ";
    const int fileCnt = tmpFiles.size();
    FILE ** files       = new FILE*[fileCnt];
    KmerPosition **entries = new KmerPosition*[fileCnt];
    size_t * entrySizes = new size_t[fileCnt];
    size_t * offsetPos  = new size_t[fileCnt];
    size_t * dataSizes  = new size_t[fileCnt];
    // init structures
    for(size_t file = 0; file < tmpFiles.size(); file++){
        files[file] = FileUtil::openFileOrDie(tmpFiles[file].c_str(),"r",true);
        size_t dataSize;
        entries[file]    = (KmerPosition*)FileUtil::mmapFile(files[file], &dataSize);
        dataSizes[file]  = dataSize;
        entrySizes[file] = dataSize/sizeof(KmerPosition);
        offsetPos[file] = 0;
    }
    std::priority_queue<FileKmer, std::vector<FileKmer>, Comp> queue;
    // read one entry for each file
    for(int file = 0; file < fileCnt; file++ ){
        size_t offset = offsetPos[file];
        if(offset + 1 < entrySizes[file]){
            queue.push(FileKmer(entries[file][offset + 1].kmer, entries[file][offset + 1].id, entries[file][offset + 1].pos, entries[file][offset + 1].seqLen, file));
        }
    }
    std::string prefResultsOutString;
    prefResultsOutString.reserve(100000000);
    FileKmer res;
    size_t prevKmer = SIZE_T_MAX;
    while(queue.empty() == false) {
        res = queue.top();
        size_t currKmer = res.kmer;
        if(TYPE == Parameters::DBTYPE_NUCLEOTIDES){
            currKmer = BIT_CLEAR(currKmer, 63);
        }
        queue.pop();
        {
            size_t offset = offsetPos[res.file];
            if(offset + 1 < entrySizes[res.file]){
                queue.push(FileKmer(entries[res.file][offset + 1].kmer, entries[res.file][offset + 1].id,
                                    entries[res.file][offset + 1].pos,  entries[res.file][offset + 1].seqLen, res.file));
                offsetPos[res.file] = offset + 1;
            }
        }
        if(prevKmer != currKmer){
            if (kmerIndex.needsFlush(res.kmer) == true) {
                kmerIndex.flush(dbw);
            }
            if(TYPE == Parameters::DBTYPE_NUCLEOTIDES){
                bool isReverse = (BIT_CHECK(currKmer, 63) == false);
                currKmer = BIT_CLEAR(currKmer, 63);
                kmerIndex.addElementSorted(currKmer, res.id, res.pos, res.seqLen, isReverse);
            }else{
                kmerIndex.addElementSorted(res.kmer, res.id, res.pos, res.seqLen, false);
            }
        }
        prevKmer = currKmer;
    }
    Debug(Debug::INFO) << "Done\n";

    kmerIndex.flush(dbw);
    dbw.writeEnd(PrefilteringIndexReader::ENTRIES, 0);
    dbw.alignToPageSize();
    // clear memory
    for(size_t file = 0; file < tmpFiles.size(); file++) {
        fclose(files[file]);
        FileUtil::munmapData((void*)entries[file], dataSizes[file]);
    }
    delete [] dataSizes;
    delete [] offsetPos;
    delete [] entries;
    delete [] entrySizes;
    delete [] files;

// write index
    Debug(Debug::INFO) << "Write ENTRIESOFFSETS (" << PrefilteringIndexReader::ENTRIESOFFSETS << ")\n";
    kmerIndex.setupOffsetTable();
    dbw.writeData((char*)kmerIndex.getOffsets(),kmerIndex.getOffsetsSize()*sizeof(size_t), PrefilteringIndexReader::ENTRIESOFFSETS, 0);
    dbw.alignToPageSize();

// write index
    Debug(Debug::INFO) << "Write ENTRIESGRIDSIZE (" << PrefilteringIndexReader::ENTRIESGRIDSIZE << ")\n";
    uint64_t gridResolution = static_cast<uint64_t >(kmerIndex.getGridResolution());
    char *gridResolutionPtr =  (char*) &gridResolution;
    dbw.writeData(gridResolutionPtr, 1 * sizeof(uint64_t), PrefilteringIndexReader::ENTRIESGRIDSIZE, 0);
    dbw.alignToPageSize();

// ENTRIESNUM
    Debug(Debug::INFO) << "Write ENTRIESNUM (" << PrefilteringIndexReader::ENTRIESNUM << ")\n";
    uint64_t entriesNum = kmerIndex.getTableEntriesNum();
    char *entriesNumPtr = (char *) &entriesNum;
    dbw.writeData(entriesNumPtr, 1 * sizeof(uint64_t), PrefilteringIndexReader::ENTRIESNUM, 0);
    dbw.alignToPageSize();

}

template void LinsearchIndexReader::mergeAndWriteIndex<0, CompareRepSequenceAndIdAndDiag>(DBWriter & dbw, std::vector<std::string> tmpFiles, int alphSize, int kmerSize);
template void LinsearchIndexReader::mergeAndWriteIndex<1, CompareRepSequenceAndIdAndDiagReverse>(DBWriter & dbw, std::vector<std::string> tmpFiles, int alphSize, int kmerSize);


template <int TYPE>
void LinsearchIndexReader::writeIndex(DBWriter & dbw,
                                      KmerPosition *hashSeqPair, size_t totalKmers,
                                      int alphSize, int kmerSize) {

    KmerIndex kmerIndex(alphSize, kmerSize);
    Debug(Debug::INFO) << "Write ENTRIES (" << PrefilteringIndexReader::ENTRIES << ")\n";
    // write entries
    dbw.writeStart(0);
    for(size_t pos = 0; pos < totalKmers && hashSeqPair[pos].kmer != SIZE_T_MAX; pos++){
        size_t kmer= hashSeqPair[pos].kmer;
        bool isReverse = false;
        if(TYPE == Parameters::DBTYPE_NUCLEOTIDES){
            isReverse = (BIT_CHECK(hashSeqPair[pos].kmer, 63) == false);
            kmer = BIT_CLEAR(kmer, 63);
        }
        if(kmerIndex.needsFlush(kmer) == true){
            kmerIndex.flush(dbw);
        }

        kmerIndex.addElementSorted(kmer, hashSeqPair[pos].id, hashSeqPair[pos].pos, hashSeqPair[pos].seqLen, isReverse);
    }
    kmerIndex.flush(dbw);
    dbw.writeEnd(PrefilteringIndexReader::ENTRIES, 0);
    dbw.alignToPageSize();

    // write index
    Debug(Debug::INFO) << "Write ENTRIESOFFSETS (" << PrefilteringIndexReader::ENTRIESOFFSETS << ")\n";
    kmerIndex.setupOffsetTable();
    dbw.writeData((char*)kmerIndex.getOffsets(),kmerIndex.getOffsetsSize()*sizeof(size_t), PrefilteringIndexReader::ENTRIESOFFSETS, 0);
    dbw.alignToPageSize();

    // write index
    Debug(Debug::INFO) << "Write ENTRIESGRIDSIZE (" << PrefilteringIndexReader::ENTRIESGRIDSIZE << ")\n";
    uint64_t gridResolution = static_cast<uint64_t >(kmerIndex.getGridResolution());
    char *gridResolutionPtr =  (char*) &gridResolution;
    dbw.writeData(gridResolutionPtr, 1 * sizeof(uint64_t), PrefilteringIndexReader::ENTRIESGRIDSIZE, 0);
    dbw.alignToPageSize();


    // ENTRIESNUM
    Debug(Debug::INFO) << "Write ENTRIESNUM (" << PrefilteringIndexReader::ENTRIESNUM << ")\n";
    uint64_t entriesNum = kmerIndex.getTableEntriesNum();
    char *entriesNumPtr = (char *) &entriesNum;
    dbw.writeData(entriesNumPtr, 1 * sizeof(uint64_t), PrefilteringIndexReader::ENTRIESNUM, 0);
    dbw.alignToPageSize();
}

template void LinsearchIndexReader::writeIndex<0>(DBWriter & dbw,
                                                  KmerPosition *hashSeqPair, size_t totalKmers,
                                                  int alphSize, int kmerSize);
template void LinsearchIndexReader::writeIndex<1>(DBWriter & dbw,
                                                  KmerPosition *hashSeqPair, size_t totalKmers,
                                                  int alphSize, int kmerSize);

std::string LinsearchIndexReader::indexName(std::string baseName) {
    std::string result(baseName);
    result.append(".").append("linidx");
    return result;
}

bool LinsearchIndexReader::checkIfIndexFile(DBReader<unsigned int> *pReader) {
    char * version = pReader->getDataByDBKey(PrefilteringIndexReader::VERSION, 0);
    if(version == NULL){
        return false;
    }
    return (strncmp(version, PrefilteringIndexReader::CURRENT_VERSION, strlen(PrefilteringIndexReader::CURRENT_VERSION)) == 0 ) ? true : false;
}

void LinsearchIndexReader::writeKmerIndexToDisk(std::string fileName, KmerPosition *kmers, size_t kmerCnt){
    FILE* filePtr = fopen(fileName.c_str(), "wb");
    if(filePtr == NULL) { perror(fileName.c_str()); EXIT(EXIT_FAILURE); }
    fwrite(kmers, sizeof(KmerPosition), kmerCnt, filePtr);
    fclose(filePtr);
}


bool LinsearchIndexReader::isIndexCompatible(DBReader<unsigned int> & index, Parameters &par, int dbtype) {
    PrefilteringIndexData meta = PrefilteringIndexReader::getMetadata(&index);
    if (meta.compBiasCorr != (par.compBiasCorrection == 1))
        return false;
    if (meta.maxSeqLength != static_cast<int>(par.maxSeqLen))
        return false;
    if (meta.seqType != dbtype)
        return false;
    if (meta.alphabetSize != par.alphabetSize)
        return false;
    if (meta.kmerSize != par.kmerSize)
        return false;
    if (meta.mask != (par.maskMode > 0))
        return false;
    if (meta.kmerThr != par.kmerScore)
        return false;
    if (meta.spacedKmer != par.spacedKmer)
        return false;
    if (par.scoringMatrixFile != PrefilteringIndexReader::getSubstitutionMatrixName(&index))
        return false;
    if (par.spacedKmerPattern != PrefilteringIndexReader::getSpacedPattern(&index))
        return false;
    if (meta.headers2 == 1 && (par.db1 != par.db2))
        return true;
    return true;
}

std::string LinsearchIndexReader::searchForIndex(std::string dbName) {
    std::string outIndexName = dbName;
    outIndexName.append(".linidx");
    if (FileUtil::fileExists(outIndexName.c_str()) == true) {
        return outIndexName;
    }
    return "";
}

#undef SIZE_T_MAX
