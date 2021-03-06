#pragma once

#include "index_calcer.h"
#include "online_predictor.h"
#include "approx_util.h"

template <bool StoreExpApprox>
inline void UpdateApproxDeltasMulti(
    const TVector<TIndexType>& indices,
    int docCount,
    TVector<TVector<double>>* leafValues, //leafValues[dimension][bucketId]
    TVector<TVector<double>>* resArr
) {
    for (int dim = 0; dim < leafValues->ysize(); ++dim) {
        ExpApproxIf(StoreExpApprox, &(*leafValues)[dim]);
        for (int z = 0; z < docCount; ++z) {
            (*resArr)[dim][z] = UpdateApprox<StoreExpApprox>((*resArr)[dim][z], (*leafValues)[dim][indices[z]]);
        }
    }
}

template <typename TError>
void AddSampleToBucketNewtonMulti(
    const TError& error,
    const TVector<double>& approx,
    float target,
    double weight,
    int iteration,
    TVector<double>* curDer,
    TArray2D<double>* curDer2,
    TSumMulti* bucket
) {
    Y_ASSERT(curDer != nullptr && curDer2 != nullptr);
    error.CalcDersMulti(approx, target, weight, curDer, curDer2);
    bucket->AddDerDer2(*curDer, *curDer2, iteration);
}

template <typename TError>
void AddSampleToBucketGradientMulti(
    const TError& error,
    const TVector<double>& approx,
    float target,
    double weight,
    int iteration,
    TVector<double>* curDer,
    TArray2D<double>* /*curDer2*/,
    TSumMulti* bucket
) {
    Y_ASSERT(curDer != nullptr);
    error.CalcDersMulti(approx, target, weight, curDer, nullptr);
    bucket->AddDerWeight(*curDer, weight, iteration);
}

template <typename TError, typename TAddSampleToBucket>
void UpdateBucketsMulti(
    TAddSampleToBucket AddSampleToBucket,
    const TVector<TIndexType>& indices,
    const TVector<float>& target,
    const TVector<float>& weight,
    const TVector<TVector<double>>& approx,
    const TVector<TVector<double>>& resArr,
    const TError& error,
    int sampleCount,
    int iteration,
    TVector<TSumMulti>* buckets
) {
    const int approxDimension = resArr.ysize();
    Y_ASSERT(approxDimension > 0);
    TVector<double> curApprox(approxDimension);
    TVector<double> bufferDer(approxDimension);
    TArray2D<double> bufferDer2(approxDimension, approxDimension);
    for (int z = 0; z < sampleCount; ++z) {
        for (int dim = 0; dim < approxDimension; ++dim) {
            curApprox[dim] = approx.empty() ? resArr[dim][z] : UpdateApprox<TError::StoreExpApprox>(approx[dim][z], resArr[dim][z]);
        }
        TSumMulti& bucket = (*buckets)[indices[z]];
        AddSampleToBucket(error, curApprox, target[z], weight.empty() ? 1 : weight[z], iteration,
                          &bufferDer, &bufferDer2, &bucket);
    }
}

template <typename TCalcModel>
void CalcMixedModelMulti(
    TCalcModel CalcModel,
    const TVector<TSumMulti>& buckets,
    int iteration,
    float l2Regularizer,
    TVector<TVector<double>>* curLeafValues
) {
    const int leafCount = buckets.ysize();
    TVector<double> avrg;
    for (int leaf = 0; leaf < leafCount; ++leaf) {
        CalcModel(buckets[leaf], iteration, l2Regularizer, &avrg);
        for (int dim = 0; dim < avrg.ysize(); ++dim) {
            (*curLeafValues)[dim][leaf] = avrg[dim];
        }
    }
}

template <typename TError, typename TCalcModel, typename TAddSampleToBucket>
void CalcApproxDeltaIterationMulti(
    TCalcModel CalcModel,
    TAddSampleToBucket AddSampleToBucket,
    const TVector<TIndexType>& indices,
    const TVector<float>& target,
    const TVector<float>& weight,
    const TFold::TBodyTail& bt,
    const TError& error,
    int iteration,
    float l2Regularizer,
    TVector<TSumMulti>* buckets,
    TVector<TVector<double>>* resArr
) {
    UpdateBucketsMulti(AddSampleToBucket, indices, target, weight, bt.Approx, *resArr, error, bt.BodyFinish, iteration, buckets);

    // compute mixed model
    const int approxDimension = resArr->ysize();
    const int leafCount = buckets->ysize();
    TVector<TVector<double>> curLeafValues(approxDimension, TVector<double>(leafCount));
    CalcMixedModelMulti(CalcModel, *buckets, iteration, l2Regularizer, &curLeafValues);
    UpdateApproxDeltasMulti<TError::StoreExpApprox>(indices, bt.BodyFinish, &curLeafValues, resArr);

    // compute tail
    TVector<double> curApprox(approxDimension);
    TVector<double> avrg(approxDimension);
    TVector<double> bufferDer(approxDimension);
    TArray2D<double> bufferDer2(approxDimension, approxDimension);
    for (int z = bt.BodyFinish; z < bt.TailFinish; ++z) {
        for (int dim = 0; dim < approxDimension; ++dim) {
            curApprox[dim] = UpdateApprox<TError::StoreExpApprox>(bt.Approx[dim][z], (*resArr)[dim][z]);
        }

        TSumMulti& bucket = (*buckets)[indices[z]];
        AddSampleToBucket(error, curApprox, target[z], weight.empty() ? 1 : weight[z], iteration,
                          &bufferDer, &bufferDer2, &bucket);

        CalcModel(bucket, iteration, l2Regularizer, &avrg);
        ExpApproxIf(TError::StoreExpApprox, &avrg);
        for (int dim = 0; dim < approxDimension; ++dim) {
            (*resArr)[dim][z] = UpdateApprox<TError::StoreExpApprox>((*resArr)[dim][z], avrg[dim]);
        }
    }
}


template <typename TError>
void CalcApproxDeltaMulti(
    const TFold& ff,
    int leafCount,
    const TError& error,
    const TVector<TIndexType>& indices,
    TLearnContext* ctx,
    TVector<TVector<TVector<double>>>* approxDelta
) {
    approxDelta->resize(ff.BodyTailArr.ysize());
    const int approxDimension = ff.GetApproxDimension();
    const auto& treeLearnerOptions = ctx->Params.ObliviousTreeOptions.Get();
    const int gradientIterations = treeLearnerOptions.LeavesEstimationIterations;
    const ELeavesEstimation estimationMethod = treeLearnerOptions.LeavesEstimationMethod;
    const float l2Regularizer = treeLearnerOptions.L2Reg;
    ctx->LocalExecutor.ExecRange([&](int bodyTailId) {
        const TFold::TBodyTail& bt = ff.BodyTailArr[bodyTailId];

        TVector<TVector<double>>& resArr = (*approxDelta)[bodyTailId];
        const double initValue = GetNeutralApprox<TError::StoreExpApprox>();
        if (resArr.empty()) {
            resArr.assign(approxDimension, TVector<double>(bt.TailFinish, initValue));
        } else {
            for (auto& arr : resArr) {
                Fill(arr.begin(), arr.end(), initValue);
            }
        }

        TVector<TSumMulti> buckets(leafCount, TSumMulti(gradientIterations, approxDimension));
        for (int it = 0; it < gradientIterations; ++it) {
            if (estimationMethod == ELeavesEstimation::Newton) {
                CalcApproxDeltaIterationMulti(CalcModelNewtonMulti, AddSampleToBucketNewtonMulti<TError>,
                                              indices, ff.LearnTarget, ff.GetLearnWeights(), bt, error, it, l2Regularizer,
                                              &buckets, &resArr);
            } else {
                Y_ASSERT(estimationMethod == ELeavesEstimation::Gradient);
                CalcApproxDeltaIterationMulti(CalcModelGradientMulti, AddSampleToBucketGradientMulti<TError>,
                                              indices, ff.LearnTarget, ff.GetLearnWeights(), bt, error, it, l2Regularizer,
                                              &buckets, &resArr);
            }
        }
    }, 0, ff.BodyTailArr.ysize(), NPar::TLocalExecutor::WAIT_COMPLETE);
}

template <typename TCalcModel, typename TAddSampleToBucket, typename TError>
void CalcLeafValuesIterationMulti(
    TCalcModel CalcModel,
    TAddSampleToBucket AddSampleToBucket,
    const TVector<TIndexType>& indices,
    const TVector<float>& target,
    const TVector<float>& weight,
    const TError& error,
    int iteration,
    float l2Regularizer,
    TVector<TSumMulti>* buckets,
    TVector<TVector<double>>* approx
) {
    int leafCount = buckets->ysize();
    int approxDimension = approx->ysize();
    int learnSampleCount = (*approx)[0].ysize();

    UpdateBucketsMulti(AddSampleToBucket, indices, target, weight, /*approx*/ TVector<TVector<double>>(), *approx, error, learnSampleCount, iteration, buckets);

    TVector<TVector<double>> curLeafValues(approxDimension, TVector<double>(leafCount));
    CalcMixedModelMulti(CalcModel, *buckets, iteration, l2Regularizer, &curLeafValues);

    UpdateApproxDeltasMulti<TError::StoreExpApprox>(indices, learnSampleCount, &curLeafValues, approx);
}

template <typename TError>
void CalcLeafValuesMulti(
    int leafCount,
    const TError& error,
    const TFold& ff,
    const TVector<TIndexType>& indices,
    TLearnContext* ctx,
    TVector<TVector<double>>* leafValues
) {
    const TFold::TBodyTail& bt = ff.BodyTailArr[0];
    const int approxDimension = ff.GetApproxDimension();

    TVector<TVector<double>> approx(approxDimension);
    for (int dim = 0; dim < approxDimension; ++dim) {
        approx[dim].assign(bt.Approx[dim].begin(), bt.Approx[dim].begin() + ff.GetLearnSampleCount());
    }

    const auto& treeLearnerOptions = ctx->Params.ObliviousTreeOptions.Get();
    const int gradientIterations = treeLearnerOptions.LeavesEstimationIterations;
    TVector<TSumMulti> buckets(leafCount, TSumMulti(gradientIterations, approxDimension));
    const ELeavesEstimation estimationMethod = treeLearnerOptions.LeavesEstimationMethod;
    const float l2Regularizer = treeLearnerOptions.L2Reg;
    for (int it = 0; it < gradientIterations; ++it) {
        if (estimationMethod == ELeavesEstimation::Newton) {
            CalcLeafValuesIterationMulti(CalcModelNewtonMulti, AddSampleToBucketNewtonMulti<TError>,
                                         indices, ff.LearnTarget, ff.GetLearnWeights(), error, it, l2Regularizer,
                                         &buckets, &approx);
        } else {
            Y_ASSERT(estimationMethod == ELeavesEstimation::Gradient);
            CalcLeafValuesIterationMulti(CalcModelGradientMulti, AddSampleToBucketGradientMulti<TError>,
                                         indices, ff.LearnTarget, ff.GetLearnWeights(), error, it, l2Regularizer,
                                         &buckets, &approx);
        }
    }

    TVector<double> avrg(approxDimension);
    leafValues->assign(approxDimension, TVector<double>(leafCount));
    for (int leaf = 0; leaf < leafCount; ++leaf) {
        for (int it = 0; it < gradientIterations; ++it) {
            if (estimationMethod == ELeavesEstimation::Newton) {
                CalcModelNewtonMulti(buckets[leaf], it, l2Regularizer, &avrg);
            } else {
                CalcModelGradientMulti(buckets[leaf], it, l2Regularizer, &avrg);
            }
            for (int dim = 0; dim < approxDimension; ++dim) {
                (*leafValues)[dim][leaf] += avrg[dim];
            }
        }
    }
}
