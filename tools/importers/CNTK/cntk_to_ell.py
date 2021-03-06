####################################################################################################
#
# Project:  Embedded Learning Library (ELL)
# File:     cntk_to_ell.py (importers)
# Authors:  Byron Changuion
#
# Requires: Python 3.x, cntk-2.0-cp35
#
####################################################################################################

import sys
import os
import logging
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../utilities/pythonlibs'))
import find_ell
import ell
import ell_utilities
import numpy as np
from cntk import load_model

# Importer
import lib.cntk_layers as cntk_layers
import lib.cntk_utilities as cntk_utilities

_logger = logging.getLogger(__name__)

def predictor_from_cntk_model(modelFile, plotModel=False):
    """Loads a CNTK model and returns an ell.neural.NeuralNetworkPredictor"""

    _logger.info("Loading...")
    z = load_model(modelFile)
    _logger.info("\nFinished loading.")

    if plotModel:
        filename = os.path.join(os.path.dirname(modelFile), os.path.basename(modelFile) + ".png")
        cntk_utilities.plot_model(z, filename)

    _logger.info("Pre-processing...")
    modelLayers = cntk_utilities.get_model_layers(z)

    # Get the relevant CNTK layers that we will convert to ELL
    layersToConvert = cntk_layers.get_filtered_layers_list(modelLayers)
    _logger.info("\nFinished pre-processing.")

    predictor = None

    try:
        # Create a list of ELL layers from the CNTK layers
        ellLayers = cntk_layers.convert_cntk_layers_to_ell_layers(
            layersToConvert)
        # Create an ELL neural network predictor from the layers
        predictor = ell.neural.FloatNeuralNetworkPredictor(ellLayers)
    except BaseException as exception:
        _logger.error("Error occurred attempting to convert cntk layers to ELL layers")
        raise exception

    return predictor
