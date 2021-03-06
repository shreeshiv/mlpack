/**
 * @file recurrent_impl.hpp
 * @author Marcus Edel
 *
 * Implementation of the LinearLayer class also known as fully-connected layer
 * or affine transformation.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#ifndef MLPACK_METHODS_ANN_LAYER_RECURRENT_IMPL_HPP
#define MLPACK_METHODS_ANN_LAYER_RECURRENT_IMPL_HPP

// In case it hasn't yet been included.
#include "recurrent.hpp"

#include "../visitor/add_visitor.hpp"
#include "../visitor/backward_visitor.hpp"
#include "../visitor/gradient_visitor.hpp"
#include "../visitor/gradient_zero_visitor.hpp"

namespace mlpack {
namespace ann /** Artificial Neural Network. */ {

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
Recurrent<InputDataType, OutputDataType, CustomLayers...>::Recurrent() :
    rho(0),
    forwardStep(0),
    backwardStep(0),
    gradientStep(0),
    deterministic(false),
    ownsLayer(false)
{
  // Nothing to do.
}

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
Recurrent<InputDataType, OutputDataType, CustomLayers...>::~Recurrent()
{
  if (ownsLayer)
  {
    boost::apply_visitor(DeleteVisitor(), recurrentModule);
    boost::apply_visitor(DeleteVisitor(), initialModule);
    boost::apply_visitor(DeleteVisitor(), startModule);
    network.clear();
  }
}

template <typename InputDataType, typename OutputDataType,
          typename... CustomLayers>
template<
    typename StartModuleType,
    typename InputModuleType,
    typename FeedbackModuleType,
    typename TransferModuleType
>
Recurrent<InputDataType, OutputDataType, CustomLayers...>::Recurrent(
    const StartModuleType& start,
    const InputModuleType& input,
    const FeedbackModuleType& feedback,
    const TransferModuleType& transfer,
    const size_t rho) :
    startModule(new StartModuleType(start)),
    inputModule(new InputModuleType(input)),
    feedbackModule(new FeedbackModuleType(feedback)),
    transferModule(new TransferModuleType(transfer)),
    rho(rho),
    forwardStep(0),
    backwardStep(0),
    gradientStep(0),
    deterministic(false),
    ownsLayer(true)
{
  initialModule = new Sequential<>();
  mergeModule = new AddMerge<>(false, false);
  recurrentModule = new Sequential<>(false);

  boost::apply_visitor(AddVisitor<CustomLayers...>(inputModule),
                       initialModule);
  boost::apply_visitor(AddVisitor<CustomLayers...>(startModule),
                       initialModule);
  boost::apply_visitor(AddVisitor<CustomLayers...>(transferModule),
                       initialModule);

  boost::apply_visitor(weightSizeVisitor, startModule);
  boost::apply_visitor(weightSizeVisitor, inputModule);
  boost::apply_visitor(weightSizeVisitor, feedbackModule);
  boost::apply_visitor(weightSizeVisitor, transferModule);

  boost::apply_visitor(AddVisitor<CustomLayers...>(inputModule), mergeModule);
  boost::apply_visitor(AddVisitor<CustomLayers...>(feedbackModule),
                       mergeModule);
  boost::apply_visitor(AddVisitor<CustomLayers...>(mergeModule),
                       recurrentModule);
  boost::apply_visitor(AddVisitor<CustomLayers...>(transferModule),
                       recurrentModule);

  network.push_back(initialModule);
  network.push_back(mergeModule);
  network.push_back(feedbackModule);
  network.push_back(recurrentModule);
}

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
template<typename eT>
void Recurrent<InputDataType, OutputDataType, CustomLayers...>::Forward(
    arma::Mat<eT>&& input, arma::Mat<eT>&& output)
{
  if (forwardStep == 0)
  {
    boost::apply_visitor(ForwardVisitor(std::move(input), std::move(output)),
        initialModule);
  }
  else
  {
    boost::apply_visitor(ForwardVisitor(std::move(input), std::move(
        boost::apply_visitor(outputParameterVisitor, inputModule))),
        inputModule);

    boost::apply_visitor(ForwardVisitor(std::move(boost::apply_visitor(
        outputParameterVisitor, transferModule)), std::move(
        boost::apply_visitor(outputParameterVisitor, feedbackModule))),
        feedbackModule);

    boost::apply_visitor(ForwardVisitor(std::move(input), std::move(output)),
        recurrentModule);
  }

  output = boost::apply_visitor(outputParameterVisitor, transferModule);

  // Save the feedback output parameter when training the module.
  if (!deterministic)
  {
    feedbackOutputParameter.push_back(output);
  }

  forwardStep++;
  if (forwardStep == rho)
  {
    forwardStep = 0;
    backwardStep = 0;

    if (!recurrentError.is_empty())
    {
      recurrentError.zeros();
    }
  }
}

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
template<typename eT>
void Recurrent<InputDataType, OutputDataType, CustomLayers...>::Backward(
    const arma::Mat<eT>&& /* input */, arma::Mat<eT>&& gy, arma::Mat<eT>&& g)
{
  if (!recurrentError.is_empty())
  {
    recurrentError += gy;
  }
  else
  {
    recurrentError = gy;
  }

  if (backwardStep < (rho - 1))
  {
    boost::apply_visitor(BackwardVisitor(std::move(boost::apply_visitor(
        outputParameterVisitor, recurrentModule)), std::move(recurrentError),
        std::move(boost::apply_visitor(deltaVisitor, recurrentModule))),
        recurrentModule);

    boost::apply_visitor(BackwardVisitor(std::move(boost::apply_visitor(
        outputParameterVisitor, inputModule)), std::move(
        boost::apply_visitor(deltaVisitor, recurrentModule)), std::move(g)),
        inputModule);

    boost::apply_visitor(BackwardVisitor(std::move(boost::apply_visitor(
        outputParameterVisitor, feedbackModule)), std::move(
        boost::apply_visitor(deltaVisitor, recurrentModule)), std::move(
        boost::apply_visitor(deltaVisitor, feedbackModule))), feedbackModule);
  }
  else
  {
    boost::apply_visitor(BackwardVisitor(std::move(boost::apply_visitor(
        outputParameterVisitor, initialModule)), std::move(recurrentError),
        std::move(g)), initialModule);
  }

  recurrentError = boost::apply_visitor(deltaVisitor, feedbackModule);
  backwardStep++;
}

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
template<typename eT>
void Recurrent<InputDataType, OutputDataType, CustomLayers...>::Gradient(
    arma::Mat<eT>&& input,
    arma::Mat<eT>&& error,
    arma::Mat<eT>&& /* gradient */)
{
  if (gradientStep < (rho - 1))
  {
    boost::apply_visitor(GradientVisitor(std::move(input), std::move(error)),
        recurrentModule);

    boost::apply_visitor(GradientVisitor(std::move(input), std::move(
        boost::apply_visitor(deltaVisitor, mergeModule))), inputModule);

    boost::apply_visitor(GradientVisitor(std::move(
        feedbackOutputParameter[feedbackOutputParameter.size() - 2 -
        gradientStep]), std::move(boost::apply_visitor(deltaVisitor,
        mergeModule))), feedbackModule);
  }
  else
  {
    boost::apply_visitor(GradientZeroVisitor(), recurrentModule);
    boost::apply_visitor(GradientZeroVisitor(), inputModule);
    boost::apply_visitor(GradientZeroVisitor(), feedbackModule);

    boost::apply_visitor(GradientVisitor(std::move(input), std::move(
        boost::apply_visitor(deltaVisitor, startModule))), initialModule);
  }

  gradientStep++;
  if (gradientStep == rho)
  {
    gradientStep = 0;
    feedbackOutputParameter.clear();
  }
}

template<typename InputDataType, typename OutputDataType,
         typename... CustomLayers>
template<typename Archive>
void Recurrent<InputDataType, OutputDataType, CustomLayers...>::serialize(
    Archive& ar, const unsigned int /* version */)
{
  // Clean up memory, if we are loading.
  if (Archive::is_loading::value)
  {
    // Clear old things, if needed.
    boost::apply_visitor(DeleteVisitor(), recurrentModule);
    boost::apply_visitor(DeleteVisitor(), initialModule);
    boost::apply_visitor(DeleteVisitor(), startModule);
    network.clear();
  }

  ar & BOOST_SERIALIZATION_NVP(startModule);
  ar & BOOST_SERIALIZATION_NVP(inputModule);
  ar & BOOST_SERIALIZATION_NVP(feedbackModule);
  ar & BOOST_SERIALIZATION_NVP(transferModule);
  ar & BOOST_SERIALIZATION_NVP(rho);
  ar & BOOST_SERIALIZATION_NVP(ownsLayer);

  // Set up the network.
  if (Archive::is_loading::value)
  {
    initialModule = new Sequential<>();
    mergeModule = new AddMerge<>(false, false);
    recurrentModule = new Sequential<>(false);

    boost::apply_visitor(AddVisitor<CustomLayers...>(inputModule),
                         initialModule);
    boost::apply_visitor(AddVisitor<CustomLayers...>(startModule),
                         initialModule);
    boost::apply_visitor(AddVisitor<CustomLayers...>(transferModule),
                         initialModule);

    boost::apply_visitor(weightSizeVisitor, startModule);
    boost::apply_visitor(weightSizeVisitor, inputModule);
    boost::apply_visitor(weightSizeVisitor, feedbackModule);
    boost::apply_visitor(weightSizeVisitor, transferModule);

    boost::apply_visitor(AddVisitor<CustomLayers...>(inputModule),
                         mergeModule);
    boost::apply_visitor(AddVisitor<CustomLayers...>(feedbackModule),
                         mergeModule);
    boost::apply_visitor(AddVisitor<CustomLayers...>(mergeModule),
                         recurrentModule);
    boost::apply_visitor(AddVisitor<CustomLayers...>(transferModule),
                         recurrentModule);

    network.push_back(initialModule);
    network.push_back(mergeModule);
    network.push_back(feedbackModule);
    network.push_back(recurrentModule);
  }
}

} // namespace ann
} // namespace mlpack

#endif
