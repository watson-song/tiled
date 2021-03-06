/*
 * tilepainter.cpp
 * Copyright 2009-2011, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 * Copyright 2009, Jeff Bland <jksb@member.fsf.org>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tilepainter.h"

#include "mapdocument.h"
#include "tilelayer.h"
#include "map.h"

using namespace Tiled;
using namespace Tiled::Internal;

namespace {

class DrawMarginsWatcher
{
public:
    DrawMarginsWatcher(MapDocument *mapDocument, TileLayer *layer)
        : mMapDocument(mapDocument)
        , mTileLayer(layer)
        , mDrawMargins(layer->drawMargins())
    {
    }

    ~DrawMarginsWatcher()
    {
        if (mTileLayer->map() == mMapDocument->map())
            if (mTileLayer->drawMargins() != mDrawMargins)
                mMapDocument->emitTileLayerDrawMarginsChanged(mTileLayer);
    }

private:
    MapDocument *mMapDocument;
    TileLayer *mTileLayer;
    const QMargins mDrawMargins;
};

} // anonymous namespace


TilePainter::TilePainter(MapDocument *mapDocument, TileLayer *tileLayer)
    : mMapDocument(mapDocument)
    , mTileLayer(tileLayer)
{
}

Cell TilePainter::cellAt(int x, int y) const
{
    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    if (!mTileLayer->contains(layerX, layerY))
        return Cell();

    return mTileLayer->cellAt(layerX, layerY);
}

void TilePainter::setCell(int x, int y, const Cell &cell)
{
    const QRegion &selection = mMapDocument->selectedArea();
    if (!(selection.isEmpty() || selection.contains(QPoint(x, y))))
        return;

    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    if (!mTileLayer->contains(layerX, layerY))
        return;

    DrawMarginsWatcher watcher(mMapDocument, mTileLayer);
    mTileLayer->setCell(layerX, layerY, cell);
    mMapDocument->emitRegionChanged(QRegion(x, y, 1, 1));
}

void TilePainter::setCells(int x, int y,
                           TileLayer *tileLayer,
                           const QRegion &mask)
{
    QRegion region = paintableRegion(x, y,
                                     tileLayer->width(),
                                     tileLayer->height());
    if (!mask.isEmpty())
        region &= mask;
    if (region.isEmpty())
        return;

    DrawMarginsWatcher watcher(mMapDocument, mTileLayer);
    mTileLayer->setCells(x - mTileLayer->x(),
                         y - mTileLayer->y(),
                         tileLayer,
                         region.translated(-mTileLayer->position()));

    mMapDocument->emitRegionChanged(region);
}

void TilePainter::drawCells(int x, int y, TileLayer *tileLayer)
{
    const QRegion region = paintableRegion(x, y,
                                           tileLayer->width(),
                                           tileLayer->height());
    if (region.isEmpty())
        return;

    DrawMarginsWatcher watcher(mMapDocument, mTileLayer);

    foreach (const QRect &rect, region.rects()) {
        for (int _y = rect.top(); _y <= rect.bottom(); ++_y) {
            for (int _x = rect.left(); _x <= rect.right(); ++_x) {
                const Cell &cell = tileLayer->cellAt(_x - x, _y - y);
                if (cell.isEmpty())
                    continue;

                mTileLayer->setCell(_x - mTileLayer->x(),
                                    _y - mTileLayer->y(),
                                    cell);
            }
        }
    }

    mMapDocument->emitRegionChanged(region);
}

void TilePainter::drawStamp(const TileLayer *stamp,
                            const QRegion &drawRegion)
{
    Q_ASSERT(stamp);
    if (stamp->bounds().isEmpty())
        return;

    const QRegion region = paintableRegion(drawRegion);
    if (region.isEmpty())
        return;

    DrawMarginsWatcher watcher(mMapDocument, mTileLayer);

    const int w = stamp->width();
    const int h = stamp->height();
    const QRect regionBounds = region.boundingRect();

    foreach (const QRect &rect, region.rects()) {
        for (int _y = rect.top(); _y <= rect.bottom(); ++_y) {
            for (int _x = rect.left(); _x <= rect.right(); ++_x) {
                const int stampX = (_x - regionBounds.left()) % w;
                const int stampY = (_y - regionBounds.top()) % h;
                const Cell &cell = stamp->cellAt(stampX, stampY);
                if (cell.isEmpty())
                    continue;

                mTileLayer->setCell(_x - mTileLayer->x(),
                                    _y - mTileLayer->y(),
                                    cell);
            }
        }
    }

    mMapDocument->emitRegionChanged(region);
}

void TilePainter::erase(const QRegion &region)
{
    const QRegion paintable = paintableRegion(region);
    if (paintable.isEmpty())
        return;

    mTileLayer->erase(paintable.translated(-mTileLayer->position()));
    mMapDocument->emitRegionChanged(paintable);
}

static QRegion fillRegion(const TileLayer *layer, QPoint fillOrigin)
{
    // Create that region that will hold the fill
    QRegion fillRegion;

    // Silently quit if parameters are unsatisfactory
    if (!layer->contains(fillOrigin))
        return fillRegion;

    // Cache cell that we will match other cells against
    const Cell matchCell = layer->cellAt(fillOrigin);

    // Grab map dimensions for later use.
    const int layerWidth = layer->width();
    const int layerHeight = layer->height();
    const int layerSize = layerWidth * layerHeight;

    // Create a queue to hold cells that need filling
    QList<QPoint> fillPositions;
    fillPositions.append(fillOrigin);

    // Create an array that will store which cells have been processed
    // This is faster than checking if a given cell is in the region/list
    QVector<quint8> processedCellsVec(layerSize);
    quint8 *processedCells = processedCellsVec.data();

    // Loop through queued positions and fill them, while at the same time
    // checking adjacent positions to see if they should be added
    while (!fillPositions.empty()) {
        const QPoint currentPoint = fillPositions.takeFirst();
        const int startOfLine = currentPoint.y() * layerWidth;

        // Seek as far left as we can
        int left = currentPoint.x();
        while (left > 0 && layer->cellAt(left - 1, currentPoint.y()) == matchCell)
            --left;

        // Seek as far right as we can
        int right = currentPoint.x();
        while (right + 1 < layerWidth && layer->cellAt(right + 1, currentPoint.y()) == matchCell)
            ++right;

        // Add cells between left and right to the region
        fillRegion += QRegion(left, currentPoint.y(), right - left + 1, 1);

        // Add cell strip to processed cells
        memset(&processedCells[startOfLine + left],
               1,
               right - left);

        // These variables cache whether the last cell was added to the queue
        // or not as an optimization, since adjacent cells on the x axis
        // do not need to be added to the queue.
        bool lastAboveCell = false;
        bool lastBelowCell = false;

        // Loop between left and right and check if cells above or
        // below need to be added to the queue
        for (int x = left; x <= right; ++x) {
            const QPoint fillPoint(x, currentPoint.y());

            // Check cell above
            if (fillPoint.y() > 0) {
                QPoint aboveCell(fillPoint.x(), fillPoint.y() - 1);
                if (!processedCells[aboveCell.y() * layerWidth + aboveCell.x()] &&
                    layer->cellAt(aboveCell) == matchCell)
                {
                    // Do not add the above cell to the queue if its
                    // x-adjacent cell was added.
                    if (!lastAboveCell)
                        fillPositions.append(aboveCell);

                    lastAboveCell = true;
                } else {
                    lastAboveCell = false;
                }

                processedCells[aboveCell.y() * layerWidth + aboveCell.x()] = 1;
            }

            // Check cell below
            if (fillPoint.y() + 1 < layerHeight) {
                QPoint belowCell(fillPoint.x(), fillPoint.y() + 1);
                if (!processedCells[belowCell.y() * layerWidth + belowCell.x()] &&
                    layer->cellAt(belowCell) == matchCell)
                {
                    // Do not add the below cell to the queue if its
                    // x-adjacent cell was added.
                    if (!lastBelowCell)
                        fillPositions.append(belowCell);

                    lastBelowCell = true;
                } else {
                    lastBelowCell = false;
                }

                processedCells[belowCell.y() * layerWidth + belowCell.x()] = 1;
            }
        }
    }

    return fillRegion;
}

QRegion TilePainter::computePaintableFillRegion(const QPoint &fillOrigin) const
{
    QRegion region = fillRegion(mTileLayer, fillOrigin - mTileLayer->position());
    region.translate(mTileLayer->position());

    const QRegion &selection = mMapDocument->selectedArea();
    if (!selection.isEmpty())
        region &= selection;

    return region;
}

QRegion TilePainter::computeFillRegion(const QPoint &fillOrigin) const
{
    QRegion region = fillRegion(mTileLayer, fillOrigin - mTileLayer->position());
    return region.translated(mTileLayer->position());
}

bool TilePainter::isDrawable(int x, int y) const
{
    const QRegion &selection = mMapDocument->selectedArea();
    if (!(selection.isEmpty() || selection.contains(QPoint(x, y))))
        return false;

    const int layerX = x - mTileLayer->x();
    const int layerY = y - mTileLayer->y();

    if (!mTileLayer->contains(layerX, layerY))
        return false;

    return true;
}

QRegion TilePainter::paintableRegion(const QRegion &region) const
{
    const QRegion bounds = QRegion(mTileLayer->bounds());
    QRegion intersection = bounds.intersected(region);

    const QRegion &selection = mMapDocument->selectedArea();
    if (!selection.isEmpty())
        intersection &= selection;

    return intersection;
}
