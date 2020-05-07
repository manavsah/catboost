#include "partial_dependence.h"

#include <catboost/private/libs/algo/apply.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/model/model.h>
#include <catboost/libs/data/data_provider.h>
#include <util/generic/utility.h>


using namespace NCB;

namespace {
    struct TFloatFeatureBucketRange {
        int featureIdx = -1;
        int start = 0;
        int end = -1;
        int numOfBuckets = 0;

        TFloatFeatureBucketRange() = default;

        TFloatFeatureBucketRange(int featureIdx, int numOfBorders)
                : featureIdx(featureIdx)
                , start(0)
                , end(numOfBorders + 1)
                , numOfBuckets(numOfBorders + 1)
        {
        }

        void update(int borderIdx, bool isGreater) {
            if (isGreater && borderIdx >= start) {
                start = borderIdx + 1;
            } else if (!isGreater && borderIdx < end) {
                end = borderIdx + 1;
            }
        }
    };
} //anonymous

TVector<TFloatFeatureBucketRange> prepareFeatureRanges(
        const TFullModel& model,
        const TVector<int>& featuresIdx
) {
    TVector<TFloatFeatureBucketRange> featureRanges(0);
    if (featuresIdx.size() < 2) {
        featureRanges.push_back(TFloatFeatureBucketRange(-1, 0));
    }
    for (int idx: featuresIdx) {
        const auto& feature = model.ModelTrees->GetFloatFeatures()[idx];
        const auto& range = TFloatFeatureBucketRange(feature.Position.Index, feature.Borders.size());
        featureRanges.push_back(range);
    }
    return featureRanges;
}

void UpdateFeatureRanges(
        const TFullModel& model,
        TVector<TFloatFeatureBucketRange>* featureRanges,
        const TVector<int>& splits,
        const TVector<ui32>& borderIdxForSplit,
        int mask
) {
    const auto& binSplits = model.ModelTrees->GetBinFeatures();

    for (size_t splitIdx = 0; splitIdx < splits.size(); ++splitIdx) {
        bool decision = (mask >> splitIdx) & 1;
        const auto& split = binSplits[splits[splitIdx]];
        for (auto& range: *featureRanges) {
            if (range.featureIdx == split.FloatFeature.FloatFeature) {
                int borderIdx = borderIdxForSplit[splits[splitIdx]];
                range.update(borderIdx, decision);
            }
        }
    }

}

std::pair<TVector<TVector<TFloatFeatureBucketRange>>, TVector<double>>  CalculateBucketRangesAndWeightsOblivious(
        const TFullModel& model,
        const TVector<int>& features,
        const TVector<ui32>& borderIdxForSplit,
        const TVector<double>& leafWeights,
        NPar::TLocalExecutor* localExecutor
) {
    const auto& binSplits = model.ModelTrees->GetBinFeatures();
    const auto& treeSplitOffsets = model.ModelTrees->GetTreeStartOffsets();
    const auto& leafOffsets = model.ModelTrees->GetFirstLeafOffsets();
    const auto& treeSizes = model.ModelTrees->GetTreeSizes();
    const auto& treeSplits = model.ModelTrees->GetTreeSplits();
    size_t leafNum = model.ModelTrees->GetLeafValues().size();

    const TVector<TFloatFeatureBucketRange> defaultRanges = prepareFeatureRanges(model, features);
    TVector<TVector<TFloatFeatureBucketRange>> leafBucketRanges(leafNum, defaultRanges);
    TVector<double> leafWeightsNew(leafWeights.size(), 0.0);

    int treeCount = model.ModelTrees->GetTreeCount();
    NPar::TLocalExecutor::TExecRangeParams blockParams(0, treeCount);
    localExecutor->ExecRange([&] (size_t treeId) {
        size_t offset = leafOffsets[treeId];
        size_t treeDepth = treeSizes[treeId];
        TVector<int> depthsToExplore;
        TVector<int> splitsToExplore;
        for (size_t depthIdx = 0; depthIdx < treeDepth; ++depthIdx) {
            int splitIdx = treeSplits[treeSplitOffsets[treeId] + depthIdx];
            const auto& split = binSplits[splitIdx];
            if (std::find(features.begin(), features.end(), split.FloatFeature.FloatFeature) != features.end()) {
                depthsToExplore.push_back(depthIdx);
                splitsToExplore.push_back(splitIdx);
            }
        }

        for (size_t leafIdx = 0; leafIdx < 1 << treeDepth; ++leafIdx) {
            for (int mask = 0; mask < 1 << depthsToExplore.size(); ++mask) {
                TVector<TFloatFeatureBucketRange> featureRanges = defaultRanges;

                int newLeafIdx = leafIdx;
                for (size_t splitIdx = 0; splitIdx < splitsToExplore.size(); ++splitIdx) {
                    int depth = depthsToExplore[splitIdx];
                    int decision = (mask >> splitIdx) & 1;
                    newLeafIdx = (newLeafIdx & ~(1UL << depth)) | (decision << depth);
                }
                UpdateFeatureRanges(model, &featureRanges, splitsToExplore, borderIdxForSplit, mask);
                leafBucketRanges[offset + newLeafIdx] = featureRanges;
                leafWeightsNew[offset + leafIdx] += leafWeights[offset + newLeafIdx];
            }
        }
    }, blockParams, NPar::TLocalExecutor::WAIT_COMPLETE);


    return std::pair(leafBucketRanges, leafWeightsNew);
}

TVector<double> MergeBucketRanges(
        const TFullModel& model,
        const TVector<int>& features,
        const TDataProvider& dataProvider,
        const TVector<TVector<TFloatFeatureBucketRange>>& leafBucketRanges,
        const TVector<double> leafWeights
) {
    const auto& leafValues = model.ModelTrees->GetLeafValues();
    TVector<TFloatFeatureBucketRange> defaultRanges = prepareFeatureRanges(model, features);
    CB_ENSURE(defaultRanges.size() == 2, "Number of features must be 2");

    int columnNum = defaultRanges[1].numOfBuckets;
    int rowNum = defaultRanges[0].numOfBuckets;
    int numOfBucketsTotal = rowNum * columnNum;
    TVector<double> edges(numOfBucketsTotal);

    for (size_t leafIdx = 0; leafIdx < leafValues.size(); ++leafIdx) {
        const auto& ranges = leafBucketRanges[leafIdx];
        double leafValue = leafValues[leafIdx];
        for (int rowIdx = ranges[0].start; rowIdx < ranges[0].end; ++rowIdx) {
            if (ranges[1].start < ranges[1].end) {
                edges[rowIdx * columnNum + ranges[1].start] += leafValue * leafWeights[leafIdx];
                if (ranges[1].end != columnNum) {
                    edges[rowIdx * columnNum + ranges[1].end] -= leafValue * leafWeights[leafIdx];
                } else if (rowIdx < rowNum - 1) {
                    edges[(rowIdx + 1) * columnNum] -= leafValue * leafWeights[leafIdx];
                }
            }
        }
    }

    TVector<double> predictionsByBuckets(numOfBucketsTotal);
    double acc = 0;
    for (int idx = 0; idx < numOfBucketsTotal; ++idx) {
        acc += edges[idx];
        predictionsByBuckets[idx] = acc;
    }

    size_t numOfDocuments = dataProvider.GetObjectCount();
    for (size_t idx = 0; idx < predictionsByBuckets.size(); ++idx) {
        predictionsByBuckets[idx] /= numOfDocuments;
    }
    return predictionsByBuckets;
}

TVector<double> CalculatePartialDependence(
        const TFullModel& model,
        const TVector<int>& features,
        const TDataProvider& dataProvider,
        const TVector<ui32>& borderIdxForSplit,
        const TVector<double> leafWeights,
        NPar::TLocalExecutor* localExecutor
) {
    const auto& [leafBucketRanges, leafWeightsNew] = CalculateBucketRangesAndWeightsOblivious(
            model,
            features,
            borderIdxForSplit,
            leafWeights,
            localExecutor
    );

    TVector<double> predictionsByBuckets = MergeBucketRanges(model, features, dataProvider, leafBucketRanges, leafWeightsNew);

    return predictionsByBuckets;
}

TVector<double> GetPartialDependence(
        const TFullModel& model,
        const TVector<int>& features,
        const NCB::TDataProviderPtr dataProvider,
        int threadCount
) {
    CB_ENSURE(model.ModelTrees->GetDimensionsCount() == 1,  "Is not supported for multiclass");
    CB_ENSURE(model.GetNumCatFeatures() == 0, "Model with categorical features are not supported");
    CB_ENSURE(features.size() > 0 && features.size() <= 2, "Number of features should be equal to one or two");

    NPar::TLocalExecutor localExecutor;
    localExecutor.RunAdditionalThreads(threadCount - 1);

    TVector<double> leafWeights = CollectLeavesStatistics(*dataProvider, model, &localExecutor);

    const auto& binSplits = model.ModelTrees->GetBinFeatures();

    TVector<ui32> borderIdxForSplit(binSplits.size(), std::numeric_limits<ui32>::infinity());
    ui32 splitIdx = 0;
    for (const auto& feature : model.ModelTrees->GetFloatFeatures()) {
        if (splitIdx == binSplits.size() ||
            binSplits[splitIdx].Type != ESplitType::FloatFeature ||
            binSplits[splitIdx].FloatFeature.FloatFeature > feature.Position.Index
                ) {
            continue;
        }
        Y_ASSERT(binSplits[splitIdx].FloatFeature.FloatFeature >= feature.Position.Index);
        for (ui32 idx = 0; idx < feature.Borders.size() && binSplits[splitIdx].FloatFeature.FloatFeature == feature.Position.Index; ++idx) {
            if (abs(binSplits[splitIdx].FloatFeature.Split - feature.Borders[idx]) < 1e-15) {
                borderIdxForSplit[splitIdx] = idx;
                ++splitIdx;
            }
        }
    }

    TVector<double> predictionsByBuckets = CalculatePartialDependence(
            model,
            features,
            *dataProvider,
            borderIdxForSplit,
            leafWeights,
            &localExecutor
    );
    return predictionsByBuckets;
}


