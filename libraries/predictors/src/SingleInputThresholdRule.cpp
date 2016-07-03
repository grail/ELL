////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Machine Learning Library (EMLL)
//  File:     SingleInputThresholdRule.cpp (predictors)
//  Authors:  Ofer Dekel
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "SingleInputThresholdRule.h"

namespace predictors
{
    SingleInputThresholdRule::SingleInputThresholdRule(size_t index, double threshold) : _index(index), _threshold(threshold)
    {}
}