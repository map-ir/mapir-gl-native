#include <mbgl/text/placement.hpp>

#include <mbgl/layout/symbol_layout.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/geometry_tile.hpp>
#include <mbgl/renderer/buckets/symbol_bucket.hpp>
#include <mbgl/renderer/bucket.hpp>
#include <mbgl/util/math.hpp>
#include <utility>

namespace mbgl {

OpacityState::OpacityState(bool placed_, bool skipFade)
    : opacity((skipFade && placed_) ? 1 : 0)
    , placed(placed_)
{
}

OpacityState::OpacityState(const OpacityState& prevState, float increment, bool placed_) :
    opacity(::fmax(0, ::fmin(1, prevState.opacity + (prevState.placed ? increment : -increment)))),
    placed(placed_) {}

bool OpacityState::isHidden() const {
    return opacity == 0 && !placed;
}

JointOpacityState::JointOpacityState(bool placedText, bool placedIcon, bool skipFade) :
    icon(OpacityState(placedIcon, skipFade)),
    text(OpacityState(placedText, skipFade)) {}

JointOpacityState::JointOpacityState(const JointOpacityState& prevOpacityState, float increment, bool placedText, bool placedIcon) :
    icon(OpacityState(prevOpacityState.icon, increment, placedIcon)),
    text(OpacityState(prevOpacityState.text, increment, placedText)) {}

bool JointOpacityState::isHidden() const {
    return icon.isHidden() && text.isHidden();
}
    
const CollisionGroups::CollisionGroup& CollisionGroups::get(const std::string& sourceID) {
    // The predicate/groupID mechanism allows for arbitrary grouping,
    // but the current interface defines one source == one group when
    // crossSourceCollisions == true.
    if (!crossSourceCollisions) {
        if (collisionGroups.find(sourceID) == collisionGroups.end()) {
            uint16_t nextGroupID = ++maxGroupID;
            collisionGroups.emplace(sourceID, CollisionGroup(
                nextGroupID,
                optional<Predicate>([nextGroupID](const IndexedSubfeature& feature) -> bool {
                    return feature.collisionGroupId == nextGroupID;
                })
            ));
        }
        return collisionGroups[sourceID];
    } else {
        static CollisionGroup nullGroup{0, nullopt};
        return nullGroup;
    }
}

Placement::Placement(const TransformState& state_, MapMode mapMode_, style::TransitionOptions transitionOptions_, const bool crossSourceCollisions, std::unique_ptr<Placement> prevPlacement_)
    : collisionIndex(state_)
    , mapMode(mapMode_)
    , transitionOptions(std::move(transitionOptions_))
    , collisionGroups(crossSourceCollisions)
    , prevPlacement(std::move(prevPlacement_))
{
    if (prevPlacement) {
        prevPlacement->prevPlacement.reset(); // Only hold on to one placement back
    }
}

void Placement::placeLayer(const RenderLayer& layer, const mat4& projMatrix, bool showCollisionBoxes) {
    std::set<uint32_t> seenCrossTileIDs;
    for (const auto& item : layer.getPlacementData()) {
        Bucket& bucket = item.bucket;
        BucketPlacementParameters params{
                item.tile,
                projMatrix,
                layer.baseImpl->source,
                item.featureIndex,
                showCollisionBoxes};
        bucket.place(*this, params, seenCrossTileIDs);
    }
}

namespace {
Point<float> calculateVariableLayoutOffset(style::SymbolAnchorType anchor, float width, float height, float radialOffset, float textBoxScale) {
    AnchorAlignment alignment = AnchorAlignment::getAnchorAlignment(anchor);
    float shiftX = -(alignment.horizontalAlign - 0.5f) * width;
    float shiftY = -(alignment.verticalAlign - 0.5f) * height;
    Point<float> offset = SymbolLayout::evaluateRadialOffset(anchor, radialOffset);
    return Point<float>(
        shiftX + offset.x * textBoxScale,
        shiftY + offset.y * textBoxScale
    );
}
} // namespace

void Placement::placeBucket(
        SymbolBucket& bucket,
        const BucketPlacementParameters& params,
        std::set<uint32_t>& seenCrossTileIDs) {
    const auto& layout = bucket.layout;
    const auto& renderTile = params.tile;
    const auto& state = collisionIndex.getTransformState();
    const float pixelsToTileUnits = renderTile.id.pixelsToTileUnits(1, state.getZoom());
    const OverscaledTileID& overscaledID = renderTile.getOverscaledTileID();
    const float scale = std::pow(2, state.getZoom() - overscaledID.overscaledZ);
    const float pixelRatio = (util::tileSize * overscaledID.overscaleFactor()) / util::EXTENT;

    mat4 posMatrix;
    state.matrixFor(posMatrix, renderTile.id);
    matrix::multiply(posMatrix, params.projMatrix, posMatrix);

    mat4 textLabelPlaneMatrix = getLabelPlaneMatrix(posMatrix,
            layout.get<style::TextPitchAlignment>() == style::AlignmentType::Map,
            layout.get<style::TextRotationAlignment>() == style::AlignmentType::Map,
            state,
            pixelsToTileUnits);

    mat4 iconLabelPlaneMatrix = getLabelPlaneMatrix(posMatrix,
            layout.get<style::IconPitchAlignment>() == style::AlignmentType::Map,
            layout.get<style::IconRotationAlignment>() == style::AlignmentType::Map,
            state,
            pixelsToTileUnits);

    const auto& collisionGroup = collisionGroups.get(params.sourceId);
    auto partiallyEvaluatedTextSize = bucket.textSizeBinder->evaluateForZoom(state.getZoom());
    auto partiallyEvaluatedIconSize = bucket.iconSizeBinder->evaluateForZoom(state.getZoom());

    optional<CollisionTileBoundaries> avoidEdges;
    if (mapMode == MapMode::Tile &&
        (layout.get<style::SymbolAvoidEdges>() ||
         layout.get<style::SymbolPlacement>() == style::SymbolPlacementType::Line)) {
        avoidEdges = collisionIndex.projectTileBoundaries(posMatrix);
    }
    
    const bool textAllowOverlap = layout.get<style::TextAllowOverlap>();
    const bool iconAllowOverlap = layout.get<style::IconAllowOverlap>();
    // This logic is similar to the "defaultOpacityState" logic below in updateBucketOpacities
    // If we know a symbol is always supposed to show, force it to be marked visible even if
    // it wasn't placed into the collision index (because some or all of it was outside the range
    // of the collision grid).
    // There is a subtle edge case here we're accepting:
    //  Symbol A has text-allow-overlap: true, icon-allow-overlap: true, icon-optional: false
    //  A's icon is outside the grid, so doesn't get placed
    //  A's text would be inside grid, but doesn't get placed because of icon-optional: false
    //  We still show A because of the allow-overlap settings.
    //  Symbol B has allow-overlap: false, and gets placed where A's text would be
    //  On panning in, there is a short period when Symbol B and Symbol A will overlap
    //  This is the reverse of our normal policy of "fade in on pan", but should look like any other
    //  collision and hopefully not be too noticeable.
    // See https://github.com/mapbox/mapbox-gl-native/issues/12683
    const bool alwaysShowText = textAllowOverlap && (iconAllowOverlap || !bucket.hasIconData() || layout.get<style::IconOptional>());
    const bool alwaysShowIcon = iconAllowOverlap && (textAllowOverlap || !bucket.hasTextData() || layout.get<style::TextOptional>());
    std::vector<style::TextVariableAnchorType> variableTextAnchors = layout.get<style::TextVariableAnchor>();
    const bool rotateWithMap = layout.get<style::TextRotationAlignment>() == style::AlignmentType::Map;
    const bool pitchWithMap = layout.get<style::TextPitchAlignment>() == style::AlignmentType::Map;
    const bool zOrderByViewportY = layout.get<style::SymbolZOrder>() == style::SymbolZOrderType::ViewportY;

    auto placeSymbol = [&] (SymbolInstance& symbolInstance) {
        if (seenCrossTileIDs.count(symbolInstance.crossTileID) != 0u) return;

        if (renderTile.holdForFade()) {
            // Mark all symbols from this tile as "not placed", but don't add to seenCrossTileIDs, because we don't
            // know yet if we have a duplicate in a parent tile that _should_ be placed.
            placements.emplace(symbolInstance.crossTileID, JointPlacement(false, false, false));
            return;
        }

        bool placeText = false;
        bool placeIcon = false;
        bool offscreen = true;
        std::pair<bool, bool> placed{ false, false };
        std::pair<bool, bool> placedVertical{ false, false };
        optional<size_t> horizontalTextIndex = symbolInstance.getDefaultHorizontalPlacedTextIndex();
        if (horizontalTextIndex) {
            PlacedSymbol& placedSymbol = bucket.text.placedSymbols.at(*horizontalTextIndex);
            const float fontSize = evaluateSizeForFeature(partiallyEvaluatedTextSize, placedSymbol);
            CollisionFeature& textCollisionFeature = symbolInstance.textCollisionFeature;

            const auto updatePreviousOrientationIfNotPlaced = [&](bool isPlaced) {
                    style::TextWritingModeType previousOrientation = style::TextWritingModeType::Horizontal;
                    if (bucket.allowVerticalPlacement && !isPlaced && prevPlacement) {
                        auto prevOrientation = prevPlacement->placedOrientations.find(symbolInstance.crossTileID);
                        if (prevOrientation != prevPlacement->placedOrientations.end()) {
                            placedOrientations[symbolInstance.crossTileID] = prevOrientation->second;
                            previousOrientation = prevOrientation->second;
                            markUsedOrientation(bucket, prevOrientation->second, symbolInstance);
                        }
                    }
                    return previousOrientation;
            };

            const auto placeTextForPlacementModes = [&] (auto& placeHorizontalFn, auto& placeVerticalFn) {
                if (bucket.allowVerticalPlacement && symbolInstance.writingModes & WritingModeType::Vertical) {
                    assert(!bucket.writingModes.empty());
                    for (auto& writingMode : bucket.writingModes) {
                        if (SymbolLayout::isVerticalWritingModeType(writingMode)) {
                            placedVertical = placed = placeVerticalFn();
                        } else {
                            placed = placeHorizontalFn();
                        }

                        if (placed.first) {
                            break;
                        }
                    }
                } else {
                    placed = placeHorizontalFn();
                }
            };

            // Line or point label placement
            if (variableTextAnchors.empty()) {
                const auto placeFeature = [&] (CollisionFeature& collisionFeature, style::TextWritingModeType orientation) {
                    auto placedFeature = collisionIndex.placeFeature(collisionFeature, {},
                                                                     posMatrix, textLabelPlaneMatrix, pixelRatio,
                                                                     placedSymbol, scale, fontSize,
                                                                     layout.get<style::TextAllowOverlap>(),
                                                                     pitchWithMap,
                                                                     params.showCollisionBoxes, avoidEdges, collisionGroup.second);
                    if (placedFeature.first) {
                        markUsedOrientation(bucket, orientation, symbolInstance);
                        placedOrientations.emplace(symbolInstance.crossTileID, orientation);
                    }
                    return placedFeature;
                };

                const auto placeHorizontal = [&] {
                    return placeFeature(symbolInstance.textCollisionFeature, style::TextWritingModeType::Horizontal);
                };

                const auto placeVertical = [&] {
                    if (bucket.allowVerticalPlacement && symbolInstance.verticalTextCollisionFeature) {
                        return placeFeature(*symbolInstance.verticalTextCollisionFeature, style::TextWritingModeType::Vertical);
                    }
                    return std::pair<bool, bool>{false, false};
                };

                placeTextForPlacementModes(placeHorizontal, placeVertical);
                updatePreviousOrientationIfNotPlaced(placed.first);

                placeText = placed.first;
                offscreen &= placed.second;
            } else if (!textCollisionFeature.alongLine && !textCollisionFeature.boxes.empty()) {
                // If this symbol was in the last placement, shift the previously used
                // anchor to the front of the anchor list.
                if (prevPlacement) {
                    auto prevOffset = prevPlacement->variableOffsets.find(symbolInstance.crossTileID);
                    if (prevOffset != prevPlacement->variableOffsets.end() &&
                        variableTextAnchors.front() != prevOffset->second.anchor) {
                        std::vector<style::TextVariableAnchorType> filtered;
                        filtered.reserve(variableTextAnchors.size());
                        filtered.push_back(prevOffset->second.anchor);
                        for (auto anchor : variableTextAnchors) {
                            if (anchor != prevOffset->second.anchor) {
                                filtered.push_back(anchor);
                            }
                        }
                        variableTextAnchors = std::move(filtered);
                    }
                }

                const auto placeFeatureForVariableAnchors = [&] (CollisionFeature& collisionFeature, style::TextWritingModeType orientation) {
                    const CollisionBox& textBox = collisionFeature.boxes[0];
                    const float width = textBox.x2 - textBox.x1;
                    const float height = textBox.y2 - textBox.y1;
                    const float textBoxScale = symbolInstance.textBoxScale;
                    std::pair<bool, bool> placedFeature = {false, false};
                    for (auto anchor : variableTextAnchors) {
                        Point<float> shift = calculateVariableLayoutOffset(anchor, width, height, symbolInstance.radialTextOffset, textBoxScale);
                        if (rotateWithMap) {
                            float angle = pitchWithMap ? state.getBearing() : -state.getBearing();
                            shift = util::rotate(shift, angle);
                        }

                        placedFeature = collisionIndex.placeFeature(collisionFeature, shift,
                                                                    posMatrix, mat4(), pixelRatio,
                                                                    placedSymbol, scale, fontSize,
                                                                    layout.get<style::TextAllowOverlap>(),
                                                                    pitchWithMap,
                                                                    params.showCollisionBoxes, avoidEdges, collisionGroup.second);
                        if (placedFeature.first) {
                            assert(symbolInstance.crossTileID != 0u);
                            optional<style::TextVariableAnchorType> prevAnchor;

                            // If this label was placed in the previous placement, record the anchor position
                            // to allow us to animate the transition
                            if (prevPlacement) {
                                auto prevOffset = prevPlacement->variableOffsets.find(symbolInstance.crossTileID);
                                auto prevPlacements = prevPlacement->placements.find(symbolInstance.crossTileID);
                                if (prevOffset != prevPlacement->variableOffsets.end() &&
                                    prevPlacements != prevPlacement->placements.end() &&
                                    prevPlacements->second.text) {
                                    // TODO: The prevAnchor seems to be unused, needs to be fixed.
                                    prevAnchor = prevOffset->second.anchor;
                                }
                            }

                            variableOffsets.insert(std::make_pair(symbolInstance.crossTileID, VariableOffset{
                                symbolInstance.radialTextOffset,
                                width,
                                height,
                                anchor,
                                textBoxScale,
                                prevAnchor
                            }));

                            markUsedJustification(bucket, anchor, symbolInstance, orientation);

                            if (bucket.allowVerticalPlacement) {
                                markUsedOrientation(bucket, orientation, symbolInstance);
                                placedOrientations.emplace(symbolInstance.crossTileID, orientation);
                            }
                            break;
                        }
                    }

                    return placedFeature;
                };

                const auto placeHorizontal = [&] {
                    return placeFeatureForVariableAnchors(symbolInstance.textCollisionFeature, style::TextWritingModeType::Horizontal);
                };

                const auto placeVertical = [&] {
                    if (bucket.allowVerticalPlacement && !placed.first && symbolInstance.verticalTextCollisionFeature) {
                        return placeFeatureForVariableAnchors(*symbolInstance.verticalTextCollisionFeature, style::TextWritingModeType::Vertical);
                    }
                    return std::pair<bool, bool>{false, false};
                };

                placeTextForPlacementModes(placeHorizontal, placeVertical);

                placeText = placed.first;
                offscreen &= placed.second;

                style::TextWritingModeType previousOrientation = updatePreviousOrientationIfNotPlaced(placed.first);

                // If we didn't get placed, we still need to copy our position from the last placement for
                // fade animations
                if (!placeText && prevPlacement) {
                    auto prevOffset = prevPlacement->variableOffsets.find(symbolInstance.crossTileID);
                    if (prevOffset != prevPlacement->variableOffsets.end()) {
                        variableOffsets[symbolInstance.crossTileID] = prevOffset->second;
                        markUsedJustification(bucket, prevOffset->second.anchor, symbolInstance, previousOrientation);
                    }
                }
            }
        }

        if (symbolInstance.placedIconIndex) {
            PlacedSymbol& placedSymbol = bucket.icon.placedSymbols.at(*symbolInstance.placedIconIndex);
            const float fontSize = evaluateSizeForFeature(partiallyEvaluatedIconSize, placedSymbol);

            auto placedIcon = collisionIndex.placeFeature(symbolInstance.iconCollisionFeature, {},
                    posMatrix, iconLabelPlaneMatrix, pixelRatio,
                    placedSymbol, scale, fontSize,
                    layout.get<style::IconAllowOverlap>(),
                    pitchWithMap,
                    params.showCollisionBoxes, avoidEdges, collisionGroup.second);
            placeIcon = placedIcon.first;
            offscreen &= placedIcon.second;
        }

        const bool iconWithoutText = !symbolInstance.hasText || layout.get<style::TextOptional>();
        const bool textWithoutIcon = !symbolInstance.hasIcon || layout.get<style::IconOptional>();

        // combine placements for icon and text
        if (!iconWithoutText && !textWithoutIcon) {
            placeText = placeIcon = placeText && placeIcon;
        } else if (!textWithoutIcon) {
            placeText = placeText && placeIcon;
        } else if (!iconWithoutText) {
            placeIcon = placeText && placeIcon;
        }

        if (placeText) {
            if (placedVertical.first && symbolInstance.verticalTextCollisionFeature) {
                collisionIndex.insertFeature(*symbolInstance.verticalTextCollisionFeature, layout.get<style::TextIgnorePlacement>(), bucket.bucketInstanceId, collisionGroup.first);
            } else {
                collisionIndex.insertFeature(symbolInstance.textCollisionFeature, layout.get<style::TextIgnorePlacement>(), bucket.bucketInstanceId, collisionGroup.first);
            }
        }

        if (placeIcon) {
            collisionIndex.insertFeature(symbolInstance.iconCollisionFeature, layout.get<style::IconIgnorePlacement>(), bucket.bucketInstanceId, collisionGroup.first);
        }

        assert(symbolInstance.crossTileID != 0);

        if (placements.find(symbolInstance.crossTileID) != placements.end()) {
            // If there's a previous placement with this ID, it comes from a tile that's fading out
            // Erase it so that the placement result from the non-fading tile supersedes it
            placements.erase(symbolInstance.crossTileID);
        }
        
        placements.emplace(symbolInstance.crossTileID, JointPlacement(placeText || alwaysShowText, placeIcon || alwaysShowIcon, offscreen || bucket.justReloaded));
        seenCrossTileIDs.insert(symbolInstance.crossTileID);
    };

    if (zOrderByViewportY) {
        const auto& sortedSymbols = bucket.getSortedSymbols(state.getBearing());
        // Place in the reverse order than draw i.e., starting from the foreground elements.
        for (auto it = sortedSymbols.rbegin(); it != sortedSymbols.rend(); ++it) {
            placeSymbol(*it);
        }
    } else {
        for (SymbolInstance& symbol : bucket.symbolInstances) {
            placeSymbol(symbol);
        }
    }

    bucket.justReloaded = false;

    // As long as this placement lives, we have to hold onto this bucket's
    // matching FeatureIndex/data for querying purposes
    retainedQueryData.emplace(std::piecewise_construct,
                                std::forward_as_tuple(bucket.bucketInstanceId),
                                std::forward_as_tuple(bucket.bucketInstanceId, params.featureIndex, overscaledID));
}

void Placement::commit(TimePoint now) {
    assert(prevPlacement);
    commitTime = now;

    bool placementChanged = false;

    float increment = mapMode == MapMode::Continuous &&
                      transitionOptions.enablePlacementTransitions &&
                      transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION) > Milliseconds(0) ?
        std::chrono::duration<float>(commitTime - prevPlacement->commitTime) / transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION) :
        1.0;

    // add the opacities from the current placement, and copy their current values from the previous placement
    for (auto& jointPlacement : placements) {
        auto prevOpacity = prevPlacement->opacities.find(jointPlacement.first);
        if (prevOpacity != prevPlacement->opacities.end()) {
            opacities.emplace(jointPlacement.first, JointOpacityState(prevOpacity->second, increment, jointPlacement.second.text, jointPlacement.second.icon));
            placementChanged = placementChanged ||
                jointPlacement.second.icon != prevOpacity->second.icon.placed ||
                jointPlacement.second.text != prevOpacity->second.text.placed;
        } else {
            opacities.emplace(jointPlacement.first, JointOpacityState(jointPlacement.second.text, jointPlacement.second.icon, jointPlacement.second.skipFade));
            placementChanged = placementChanged || jointPlacement.second.icon || jointPlacement.second.text;
        }
    }

    // copy and update values from the previous placement that aren't in the current placement but haven't finished fading
    for (auto& prevOpacity : prevPlacement->opacities) {
        if (opacities.find(prevOpacity.first) == opacities.end()) {
            JointOpacityState jointOpacity(prevOpacity.second, increment, false, false);
            if (!jointOpacity.isHidden()) {
                opacities.emplace(prevOpacity.first, jointOpacity);
                placementChanged = placementChanged || prevOpacity.second.icon.placed || prevOpacity.second.text.placed;
            }
        }
    }

    for (auto& prevOffset : prevPlacement->variableOffsets) {
        const uint32_t crossTileID = prevOffset.first;
        auto foundOffset = variableOffsets.find(crossTileID);
        auto foundOpacity = opacities.find(crossTileID);
        if (foundOffset == variableOffsets.end() && foundOpacity != opacities.end() && !foundOpacity->second.isHidden()) {
            variableOffsets[prevOffset.first] = prevOffset.second;
        }
    }

    for (auto& prevOrientation : prevPlacement->placedOrientations) {
        const uint32_t crossTileID = prevOrientation.first;
        auto foundOrientation = placedOrientations.find(crossTileID);
        auto foundOpacity = opacities.find(crossTileID);
        if (foundOrientation == placedOrientations.end() && foundOpacity != opacities.end() && !foundOpacity->second.isHidden()) {
            placedOrientations[prevOrientation.first] = prevOrientation.second;
        }
    }

    fadeStartTime = placementChanged ? commitTime : prevPlacement->fadeStartTime;
}

void Placement::updateLayerBuckets(const RenderLayer& layer, const TransformState& state, bool updateOpacities) {
    std::set<uint32_t> seenCrossTileIDs;
    for (const auto& item : layer.getPlacementData()) {
        item.bucket.get().updateVertices(*this, updateOpacities, state, item.tile, seenCrossTileIDs);
    }
}

namespace {
Point<float> calculateVariableRenderShift(style::SymbolAnchorType anchor, float width, float height, float radialOffset, float textBoxScale, float renderTextSize) {
    AnchorAlignment alignment = AnchorAlignment::getAnchorAlignment(anchor);
    float shiftX = -(alignment.horizontalAlign - 0.5f) * width;
    float shiftY = -(alignment.verticalAlign - 0.5f) * height;
    Point<float> offset = SymbolLayout::evaluateRadialOffset(anchor, radialOffset);
    return { (shiftX / textBoxScale + offset.x) * renderTextSize,
             (shiftY / textBoxScale + offset.y) * renderTextSize };
}
} // namespace

bool Placement::updateBucketDynamicVertices(SymbolBucket& bucket, const TransformState& state, const RenderTile& tile) {
    using namespace style;
    const auto& layout = bucket.layout;
    const bool alongLine = layout.get<SymbolPlacement>() != SymbolPlacementType::Point;
    bool result = false;

    if (alongLine) {
        if (bucket.hasIconData() && layout.get<IconRotationAlignment>() == AlignmentType::Map) {
            const bool pitchWithMap = layout.get<style::IconPitchAlignment>() == style::AlignmentType::Map;
            const bool keepUpright = layout.get<style::IconKeepUpright>();
            reprojectLineLabels(bucket.icon.dynamicVertices, bucket.icon.placedSymbols,
                tile.matrix, pitchWithMap, true /*rotateWithMap*/, keepUpright,
                tile, *bucket.iconSizeBinder, state);
            result = true;
        }

        if (bucket.hasTextData() && layout.get<TextRotationAlignment>() == AlignmentType::Map) {
            const bool pitchWithMap = layout.get<style::TextPitchAlignment>() == style::AlignmentType::Map;
            const bool keepUpright = layout.get<style::TextKeepUpright>();
            reprojectLineLabels(bucket.text.dynamicVertices, bucket.text.placedSymbols,
                tile.matrix, pitchWithMap, true /*rotateWithMap*/, keepUpright,
                tile, *bucket.textSizeBinder, state);
            result = true;
        }
    } else if (!layout.get<TextVariableAnchor>().empty() && bucket.hasTextData()) {
        bucket.text.dynamicVertices.clear();
        bucket.hasVariablePlacement = false;

        const auto partiallyEvaluatedSize = bucket.textSizeBinder->evaluateForZoom(state.getZoom());
        const float tileScale = std::pow(2, state.getZoom() - tile.getOverscaledTileID().overscaledZ);
        const bool rotateWithMap = layout.get<TextRotationAlignment>() == AlignmentType::Map;
        const bool pitchWithMap = layout.get<TextPitchAlignment>() == AlignmentType::Map;
        const float pixelsToTileUnits = tile.id.pixelsToTileUnits(1.0, state.getZoom());
        const auto labelPlaneMatrix = getLabelPlaneMatrix(tile.matrix, pitchWithMap, rotateWithMap, state, pixelsToTileUnits);

        for (const PlacedSymbol& symbol : bucket.text.placedSymbols) {
            optional<VariableOffset> variableOffset;
            const bool skipOrientation = bucket.allowVerticalPlacement && !symbol.placedOrientation;
            if (!symbol.hidden && symbol.crossTileID != 0u && !skipOrientation) {
                auto it = variableOffsets.find(symbol.crossTileID);
                if (it != variableOffsets.end()) {
                    bucket.hasVariablePlacement = true;
                    variableOffset = it->second;
                }
            }

            if (!variableOffset) {
                // These symbols are from a justification that is not being used, or a label that wasn't placed
                // so we don't need to do the extra math to figure out what incremental shift to apply.
                hideGlyphs(symbol.glyphOffsets.size(), bucket.text.dynamicVertices);
            } else {
                const Point<float> tileAnchor = symbol.anchorPoint;
                const auto projectedAnchor = project(tileAnchor, pitchWithMap ? tile.matrix : labelPlaneMatrix);
                const float perspectiveRatio = 0.5f + 0.5f * (state.getCameraToCenterDistance() / projectedAnchor.second);
                float renderTextSize = evaluateSizeForFeature(partiallyEvaluatedSize, symbol) * perspectiveRatio / util::ONE_EM;
                if (pitchWithMap) {
                    // Go from size in pixels to equivalent size in tile units
                    renderTextSize *= bucket.tilePixelRatio / tileScale;
                }

                auto shift = calculateVariableRenderShift(
                        (*variableOffset).anchor,
                        (*variableOffset).width,
                        (*variableOffset).height,
                        (*variableOffset).radialOffset,
                        (*variableOffset).textBoxScale,
                        renderTextSize);

                // Usual case is that we take the projected anchor and add the pixel-based shift
                // calculated above. In the (somewhat weird) case of pitch-aligned text, we add an equivalent
                // tile-unit based shift to the anchor before projecting to the label plane.
                Point<float> shiftedAnchor;
                if (pitchWithMap) {
                    shiftedAnchor = project(Point<float>(tileAnchor.x + shift.x, tileAnchor.y + shift.y),
                                            labelPlaneMatrix).first;
                } else {
                    if (rotateWithMap) {
                        auto rotated = util::rotate(shift, -state.getPitch());
                        shiftedAnchor = Point<float>(projectedAnchor.first.x + rotated.x,
                                                    projectedAnchor.first.y + rotated.y);
                    } else {
                        shiftedAnchor = Point<float>(projectedAnchor.first.x + shift.x,
                                                    projectedAnchor.first.y + shift.y);
                    }
                }

                for (std::size_t i = 0; i < symbol.glyphOffsets.size(); ++i) {
                    addDynamicAttributes(shiftedAnchor, symbol.angle, bucket.text.dynamicVertices);
                }
            }
        }

        result = true;
    } else if (bucket.allowVerticalPlacement && bucket.hasTextData()) {
        bucket.text.dynamicVertices.clear();
        for (const PlacedSymbol& symbol : bucket.text.placedSymbols) {
            if (symbol.hidden || !symbol.placedOrientation) {
                hideGlyphs(symbol.glyphOffsets.size(), bucket.text.dynamicVertices);
            } else {
                 for (std::size_t i = 0; i < symbol.glyphOffsets.size(); ++i) {
                    addDynamicAttributes(symbol.anchorPoint, symbol.angle, bucket.text.dynamicVertices);
                }
            }
        }
        result = true;
    }

    return result;
}

void Placement::updateBucketOpacities(SymbolBucket& bucket, const TransformState& state, std::set<uint32_t>& seenCrossTileIDs) {
    if (bucket.hasTextData()) bucket.text.opacityVertices.clear();
    if (bucket.hasIconData()) bucket.icon.opacityVertices.clear();
    if (bucket.hasCollisionBoxData()) bucket.collisionBox.dynamicVertices.clear();
    if (bucket.hasCollisionCircleData()) bucket.collisionCircle.dynamicVertices.clear();

    JointOpacityState duplicateOpacityState(false, false, true);

    const bool textAllowOverlap = bucket.layout.get<style::TextAllowOverlap>();
    const bool iconAllowOverlap = bucket.layout.get<style::IconAllowOverlap>();
    const bool variablePlacement = !bucket.layout.get<style::TextVariableAnchor>().empty();
    const bool rotateWithMap = bucket.layout.get<style::TextRotationAlignment>() == style::AlignmentType::Map;
    const bool pitchWithMap = bucket.layout.get<style::TextPitchAlignment>() == style::AlignmentType::Map;

    // If allow-overlap is true, we can show symbols before placement runs on them
    // But we have to wait for placement if we potentially depend on a paired icon/text
    // with allow-overlap: false.
    // See https://github.com/mapbox/mapbox-gl-native/issues/12483
    JointOpacityState defaultOpacityState(
            textAllowOverlap && (iconAllowOverlap || !bucket.hasIconData() || bucket.layout.get<style::IconOptional>()),
            iconAllowOverlap && (textAllowOverlap || !bucket.hasTextData() || bucket.layout.get<style::TextOptional>()),
            true);

    for (SymbolInstance& symbolInstance : bucket.symbolInstances) {
        bool isDuplicate = seenCrossTileIDs.count(symbolInstance.crossTileID) > 0;

        auto it = opacities.find(symbolInstance.crossTileID);
        auto opacityState = defaultOpacityState;
        if (isDuplicate) {
            opacityState = duplicateOpacityState;
        } else if (it != opacities.end()) {
            opacityState = it->second;
        }

        if (it == opacities.end()) {
            opacities.emplace(symbolInstance.crossTileID, defaultOpacityState);
        }

        seenCrossTileIDs.insert(symbolInstance.crossTileID);

        if (symbolInstance.hasText) {
            auto opacityVertex = SymbolSDFTextProgram::opacityVertex(opacityState.text.placed, opacityState.text.opacity);
            if (symbolInstance.placedRightTextIndex) {
                for (size_t i = 0; i < symbolInstance.rightJustifiedGlyphQuads.size() * 4; i++) {
                    bucket.text.opacityVertices.emplace_back(opacityVertex);
                }
                PlacedSymbol& placed = bucket.text.placedSymbols[*symbolInstance.placedRightTextIndex];
                placed.hidden = opacityState.isHidden();
            }
            if (symbolInstance.placedCenterTextIndex && !symbolInstance.singleLine) {
                for (size_t i = 0; i < symbolInstance.centerJustifiedGlyphQuads.size() * 4; i++) {
                    bucket.text.opacityVertices.emplace_back(opacityVertex);
                }
                PlacedSymbol& placed = bucket.text.placedSymbols[*symbolInstance.placedCenterTextIndex];
                placed.hidden = opacityState.isHidden();
            }
            if (symbolInstance.placedLeftTextIndex && !symbolInstance.singleLine) {
                for (size_t i = 0; i < symbolInstance.leftJustifiedGlyphQuads.size() * 4; i++) {
                    bucket.text.opacityVertices.emplace_back(opacityVertex);
                }
                PlacedSymbol& placed = bucket.text.placedSymbols[*symbolInstance.placedLeftTextIndex];
                placed.hidden = opacityState.isHidden();
            }
            if (symbolInstance.placedVerticalTextIndex) {
                for (size_t i = 0; i < symbolInstance.verticalGlyphQuads.size() * 4; i++) {
                    bucket.text.opacityVertices.emplace_back(opacityVertex);
                }
                PlacedSymbol& placed = bucket.text.placedSymbols[*symbolInstance.placedVerticalTextIndex];
                placed.hidden = opacityState.isHidden();
            }

            style::TextWritingModeType previousOrientation = style::TextWritingModeType::Horizontal;
            if (bucket.allowVerticalPlacement) {
                auto prevOrientation = placedOrientations.find(symbolInstance.crossTileID);
                if (prevOrientation != placedOrientations.end()) {
                    previousOrientation = prevOrientation->second;
                    markUsedOrientation(bucket, prevOrientation->second, symbolInstance);
                }
            }

            auto prevOffset = variableOffsets.find(symbolInstance.crossTileID);
            if (prevOffset != variableOffsets.end()) {
                markUsedJustification(bucket, prevOffset->second.anchor, symbolInstance, previousOrientation);
            }
        }
        if (symbolInstance.hasIcon) {
            auto opacityVertex = SymbolIconProgram::opacityVertex(opacityState.icon.placed, opacityState.icon.opacity);
            if (symbolInstance.iconQuad) {
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
            }
            if (symbolInstance.placedIconIndex) {
                bucket.icon.placedSymbols[*symbolInstance.placedIconIndex].hidden = opacityState.isHidden();
            }
        }
        
        auto updateCollisionBox = [&](const auto& feature, const bool placed) {
            if (feature.alongLine) {
                return;
            }
            auto dynamicVertex = CollisionBoxProgram::dynamicVertex(placed, false, {});
            for (size_t i = 0; i < feature.boxes.size() * 4; i++) {
                bucket.collisionBox.dynamicVertices.emplace_back(dynamicVertex);
            }
        };

        auto updateCollisionTextBox = [this, &bucket, &symbolInstance, &state, variablePlacement, rotateWithMap, pitchWithMap](const auto& feature, const bool placed) {
            if (feature.alongLine) {
                return;
            }
            Point<float> shift;
            bool used = true;
            if (variablePlacement) {
                auto foundOffset = variableOffsets.find(symbolInstance.crossTileID);
                if (foundOffset != variableOffsets.end()) {
                    const VariableOffset& variableOffset = foundOffset->second;
                    // This will show either the currently placed position or the last
                    // successfully placed position (so you can visualize what collision
                    // just made the symbol disappear, and the most likely place for the
                    // symbol to come back)
                    shift = calculateVariableLayoutOffset(variableOffset.anchor,
                                                          variableOffset.width,
                                                          variableOffset.height,
                                                          variableOffset.radialOffset,
                                                          variableOffset.textBoxScale);
                    if (rotateWithMap) {
                        shift = util::rotate(shift, pitchWithMap ? state.getBearing() : -state.getBearing());
                    }
                } else {
                    // No offset -> this symbol hasn't been placed since coming on-screen
                    // No single box is particularly meaningful and all of them would be too noisy
                    // Use the center box just to show something's there, but mark it "not used"
                    used = false;
                }
            }
            auto dynamicVertex = CollisionBoxProgram::dynamicVertex(placed, !used, shift);
            for (size_t i = 0; i < feature.boxes.size() * 4; i++) {
                bucket.collisionBox.dynamicVertices.emplace_back(dynamicVertex);
            }
        };
        
        auto updateCollisionCircles = [&](const auto& feature, const bool placed) {
            if (!feature.alongLine) {
                return;
            }
            for (const CollisionBox& box : feature.boxes) {
                auto dynamicVertex = CollisionBoxProgram::dynamicVertex(placed, !box.used, {});
                bucket.collisionCircle.dynamicVertices.emplace_back(dynamicVertex);
                bucket.collisionCircle.dynamicVertices.emplace_back(dynamicVertex);
                bucket.collisionCircle.dynamicVertices.emplace_back(dynamicVertex);
                bucket.collisionCircle.dynamicVertices.emplace_back(dynamicVertex);
            }
        };
        
        if (bucket.hasCollisionBoxData()) {
            // TODO: update collision box opacity based on selected text variant (horizontal | vertical).
            updateCollisionTextBox(symbolInstance.textCollisionFeature, opacityState.text.placed);
            if (bucket.allowVerticalPlacement && symbolInstance.verticalTextCollisionFeature) {
                updateCollisionTextBox(*symbolInstance.verticalTextCollisionFeature, opacityState.text.placed);
            }
            updateCollisionBox(symbolInstance.iconCollisionFeature, opacityState.icon.placed);
        }
        if (bucket.hasCollisionCircleData()) {
            updateCollisionCircles(symbolInstance.textCollisionFeature, opacityState.text.placed);
            updateCollisionCircles(symbolInstance.iconCollisionFeature, opacityState.icon.placed);
        }
    }

    bucket.sortFeatures(state.getBearing());
    auto retainedData = retainedQueryData.find(bucket.bucketInstanceId);
    if (retainedData != retainedQueryData.end()) {
        retainedData->second.featureSortOrder = bucket.featureSortOrder;
    }
}

void Placement::markUsedJustification(SymbolBucket& bucket,
                                      style::TextVariableAnchorType placedAnchor,
                                      SymbolInstance& symbolInstance,
                                      style::TextWritingModeType orientation) {

    const auto justificationToIndex = [&symbolInstance, orientation] (style::TextJustifyType type) -> optional<std::size_t> {
        // Vertical symbol has just one justification, style::TextJustifyType::Left.
        if (orientation == style::TextWritingModeType::Vertical) {
            return symbolInstance.placedVerticalTextIndex;
        }

        switch (type) {
            case style::TextJustifyType::Center:
                return symbolInstance.placedCenterTextIndex;
            case style::TextJustifyType::Left:
                return symbolInstance.placedLeftTextIndex;
            case style::TextJustifyType::Right:
                return symbolInstance.placedRightTextIndex;
            default:
                assert(false);
        }
        return nullopt;
    };

    std::array<optional<size_t>, 4> indexes {{symbolInstance.placedCenterTextIndex,
                                              symbolInstance.placedLeftTextIndex,
                                              symbolInstance.placedRightTextIndex,
                                              symbolInstance.placedVerticalTextIndex}};

    style::TextJustifyType justify = getAnchorJustification(placedAnchor);
    assert(justify == style::TextJustifyType::Right || justify == style::TextJustifyType::Center || justify == style::TextJustifyType::Left);
    const optional<size_t> autoIndex = justificationToIndex(justify);

    for (auto& index : indexes) {
        if (index) {
            assert(bucket.text.placedSymbols.size() > *index);
            if (autoIndex && *index != *autoIndex) {
                // There are multiple justifications and this one isn't it: shift offscreen
                bucket.text.placedSymbols.at(*index).crossTileID = 0u;
            } else {
                // Either this is the chosen justification or the justification is hardwired: use this one
                bucket.text.placedSymbols.at(*index).crossTileID = symbolInstance.crossTileID;
            }
        }
    }
}

void Placement::markUsedOrientation(SymbolBucket& bucket, style::TextWritingModeType orientation, SymbolInstance& symbolInstance) {
    auto horizontal = orientation == style::TextWritingModeType::Horizontal ?
                                     optional<style::TextWritingModeType>(orientation) : nullopt;
    auto vertical = orientation == style::TextWritingModeType::Vertical ?
                                     optional<style::TextWritingModeType>(orientation) : nullopt;

    if (symbolInstance.placedRightTextIndex) {
        bucket.text.placedSymbols.at(*symbolInstance.placedRightTextIndex).placedOrientation = horizontal;
    }

    if (symbolInstance.placedCenterTextIndex && !symbolInstance.singleLine) {
        bucket.text.placedSymbols.at(*symbolInstance.placedCenterTextIndex).placedOrientation = horizontal;
    }

    if (symbolInstance.placedLeftTextIndex && !symbolInstance.singleLine) {
        bucket.text.placedSymbols.at(*symbolInstance.placedLeftTextIndex).placedOrientation = horizontal;
    }

    if (symbolInstance.placedVerticalTextIndex) {
        bucket.text.placedSymbols.at(*symbolInstance.placedVerticalTextIndex).placedOrientation = vertical;
    }
}

float Placement::symbolFadeChange(TimePoint now) const {
    if (mapMode == MapMode::Continuous && transitionOptions.enablePlacementTransitions &&
        transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION) > Milliseconds(0)) {
        return std::chrono::duration<float>(now - commitTime) / transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION);
    } else {
        return 1.0;
    }
}

bool Placement::hasTransitions(TimePoint now) const {
    if (mapMode == MapMode::Continuous && transitionOptions.enablePlacementTransitions) {
        return stale || std::chrono::duration<float>(now - fadeStartTime) < transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION);
    } else {
        return false;
    }
}

bool Placement::stillRecent(TimePoint now) const {
    // Even if transitionOptions.duration is set to a value < 300ms, we still wait for this default transition duration
    // before attempting another placement operation.
    return mapMode == MapMode::Continuous &&
        transitionOptions.enablePlacementTransitions &&
        commitTime + std::max(util::DEFAULT_TRANSITION_DURATION, transitionOptions.duration.value_or(util::DEFAULT_TRANSITION_DURATION)) > now;
}

void Placement::setStale() {
    stale = true;
}

const CollisionIndex& Placement::getCollisionIndex() const {
    return collisionIndex;
}
    
const RetainedQueryData& Placement::getQueryData(uint32_t bucketInstanceId) const {
    auto it = retainedQueryData.find(bucketInstanceId);
    if (it == retainedQueryData.end()) {
        throw std::runtime_error("Placement::getQueryData with unrecognized bucketInstanceId");
    }
    return it->second;
}

} // namespace mbgl
