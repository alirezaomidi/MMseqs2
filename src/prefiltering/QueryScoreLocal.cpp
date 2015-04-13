//
//  QueryScoreLocal.cpp
//
//  Created by Martin Steinegger on 03.06.13.
//  Copyright (c) 2013 Martin Steinegger. All rights reserved.
//
#include <random>
#include "QueryScoreLocal.h"
#include "QueryScore.h"

QueryScoreLocal::QueryScoreLocal(size_t dbSize, unsigned int *seqLens, int k, short kmerThr, double kmerMatchProb)
: QueryScore(dbSize, seqLens, k, kmerThr, kmerMatchProb)    // Call the QueryScore constructor
{
    this->scoreSizes = new unsigned int[SCORE_RANGE];
    memset(scoreSizes, 0, SCORE_RANGE * sizeof(unsigned int));
    this->logMatchProb = log(kmerMatchProb);
    this->logScoreFactorial = new double[SCORE_RANGE];

    // pre compute the factorial of all possible scores
    logScoreFactorial[0] = log(1.0);
    for(size_t score = 1; score < SCORE_RANGE; score++){
        const double scoreDbl = static_cast<double>(score);

        const double S_fact = std::min(std::numeric_limits<double>::max(),
                             sqrt(2 * M_PI * scoreDbl) * pow(scoreDbl / exp(1), scoreDbl) * exp(1 / (12  * scoreDbl)));
        logScoreFactorial[score] = log(S_fact);
    }
}

QueryScoreLocal::~QueryScoreLocal(){
    delete [] scoreSizes;
    delete [] logScoreFactorial;
}

void QueryScoreLocal::reset() {
//    memset (scores_128, 0, scores_128_size * 2);
    scoresSum = 0;
    numMatches = 0;
    memset(scoreSizes, 0, SCORE_RANGE * sizeof(unsigned int));
}

std::pair<hit_t *, size_t> QueryScoreLocal::getResult(const unsigned int querySeqLen, unsigned int scoreThr, const unsigned int identityId) {
    // adjust score threshold because we use > for check later
    scoreThr = (scoreThr == 0 ) ? 0 : scoreThr - 1;
    simd_int thr = simdi16_set( scoreThr );
    simd_int zero =  simdi_setzero();
    const size_t lenght = scores_128_size / (SIMD_SHORT_SIZE); // *2for short
    simd_int* __restrict s   = scores_128;
    unsigned int elementCounter = 0;

    // check if there is the identity of the query sequence in the database
    // the identity should be included in the results
    if (identityId != UINT_MAX){
        elementCounter++;
    }

    for (size_t pos = 0; pos < lenght; pos++ ){
        // look for entries above the threshold
        simd_int currElements = simdi_load(s + pos);
        // shift out the diagonal information and move the score to the first byte
        currElements = simdi16_srli(currElements, 8);
        simd_int cmp = simdi16_gt(currElements, thr); // currElement > thr
        const unsigned int cmp_set_bits = simdi8_movemask(cmp);
        // set zero so no memset is needed
        simdi_store(s + pos, zero);
        if (cmp_set_bits != 0){
            for(size_t i = 0; i < SIMD_SHORT_SIZE; i++){
                if( CHECK_BIT(cmp_set_bits,i*2) && pos * SIMD_SHORT_SIZE + i != identityId) {
                    unsigned short rawScore = simdi16_extract (currElements,  i);
                    hit_t * result = (resList + elementCounter);
                    result->seqId = pos * SIMD_SHORT_SIZE + i;
                    //result->zScore = (((float)rawScore) - mu )/ sqrt(mu);
                    result->zScore = -computeLogProbabiliy(rawScore, seqLens[result->seqId]);
                    //std::cout << result->zScore << std::endl;

                    //result->zScore = (rawScore);
                    result->prefScore = rawScore;
                    //scoreSizes += rawScore;
                    elementCounter++;
                    if(elementCounter >= MAX_RES_LIST_LEN){
                        // because the memset will not be finished
                        memset (s + pos, 0, scores_128_size * 2);
                        goto OuterLoop;
                    }
                }
            }
        }
    }
    OuterLoop:
    // sort hits by score
    // include the identity in results if its there
    if (identityId != UINT_MAX){
        hit_t * result = (resList + 0);
        const unsigned char * data =  (unsigned char *) scores;
        const unsigned int seqIndex = identityId * 2;
        unsigned char rawScore  = data[seqIndex + 1];
        rawScore = (rawScore == 0) ? 1 : rawScore;
        result->zScore = -computeLogProbabiliy(rawScore, seqLens[result->seqId]);
        result->seqId = identityId;
        result->prefScore = rawScore;
        std::sort(resList + 1, resList + elementCounter, compareHits);
    }
    else{
        std::sort(resList, resList + elementCounter, compareHits);
    }
    //std::sort(resList, resList + elementCounter, compareHits);
    std::pair<hit_t *, size_t>  pair = std::make_pair(this->resList, elementCounter);
    return pair;
}

void QueryScoreLocal::updateScoreBins() {
    unsigned char * data =  (unsigned char *) scores;
    for(size_t i = 0; i < dbSize; i++){
        scoreSizes[data[(i * 2) + 1]]++;
    }
}

unsigned int QueryScoreLocal::computeScoreThreshold(size_t maxHitsPerQuery) {
    size_t foundHits = 0;
    size_t scoreThr = 0;
    for(scoreThr = SCORE_RANGE - 1; scoreThr > 0 ; scoreThr--){
        foundHits += scoreSizes[scoreThr];
        if(foundHits >= maxHitsPerQuery)
            break;
    }
    return scoreThr;
}

inline double QueryScoreLocal::computeLogProbabiliy(const unsigned short rawScore, const unsigned int dbSeqLen) {
    const double score = static_cast<double>(rawScore);
    const float mu = kmerMatchProb * dbSeqLen;
    const double mid_term = score * (logMatchProb + log(dbSeqLen));
    const double first_term = -(mu * score /(score + 1));
    return first_term + mid_term - logScoreFactorial[rawScore];
}
