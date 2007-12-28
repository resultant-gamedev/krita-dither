/*
 * This file is part of the KDE project
 *
 *  Copyright (c) 2007 Cyrille Berger <cberger@cberger.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "Dither.h"

#include <stdlib.h>
#include <vector>

#include <klocale.h>
#include <kiconloader.h>
#include <kinstance.h>
#include <kmessagebox.h>
#include <kstandarddirs.h>
#include <ktempfile.h>
#include <kdebug.h>
#include <kgenericfactory.h>

#include <kis_multi_double_filter_widget.h>
#include <kis_iterators_pixel.h>
#include <kis_progress_display_interface.h>
#include <kis_filter_registry.h>
#include <kis_global.h>
#include <kis_transaction.h>
#include <kis_types.h>
#include <kis_selection.h>

#include <kis_convolution_painter.h>

#include <qimage.h>
#include <qpixmap.h>
#include <qbitmap.h>
#include <qpainter.h>
#include <qcombobox.h>

#include "DitherConfigurationWidget.h"
#include "DitherConfigurationBaseWidget.h"

typedef Q_UINT8 quint8;

typedef KGenericFactory<KritaDither> KritaDitherFactory;
K_EXPORT_COMPONENT_FACTORY( kritaDither, KritaDitherFactory( "krita" ) )

KritaDither::KritaDither(QObject *parent, const char *name, const QStringList &)
: KParts::Plugin(parent, name)
{
    setInstance(KritaDitherFactory::instance());

    kdDebug(41006) << "Dither filter plugin. Class: "
    << className()
    << ", Parent: "
    << parent -> className()
    << "\n";

    if(parent->inherits("KisFilterRegistry"))
    {
        KisFilterRegistry * manager = dynamic_cast<KisFilterRegistry *>(parent);
        manager->add(new KisDitherFilter());
    }
}

KritaDither::~KritaDither()
{
}

KisDitherFilter::KisDitherFilter() 
    : KisFilter(id(), "dither", i18n("&Dither"))
{
}

KisFilterConfiguration* KisDitherFilter::configuration()
{
    KisFilterConfiguration* config = new KisFilterConfiguration(id().id(),1);
    config->setProperty("paletteSize", 16);
    config->setProperty("paletteType", 0);
    return config;
};

KisFilterConfigWidget * KisDitherFilter::createConfigurationWidget(QWidget* parent, KisPaintDeviceSP /*dev*/)
{
    DitherConfigurationWidget* w = new DitherConfigurationWidget(parent, "");
    Q_CHECK_PTR(w);
    return w;
}

KisFilterConfiguration* KisDitherFilter::configuration(QWidget* nwidget)
{
    DitherConfigurationWidget* widget = (DitherConfigurationWidget*) nwidget;
    if( widget == 0 )
    {
        return configuration();
    } else {
        DitherConfigurationBaseWidget* baseWidget = widget->widget();
        KisFilterConfiguration* config = new KisFilterConfiguration(id().id(),1);
        config->setProperty("paletteSize", baseWidget->paletteSize->value() );
        config->setProperty("paletteType", baseWidget->paletteType->currentItem() );
        return config;
    }
}

bool operator<(const QColor& c1, const QColor& c2)
{
    if(c1.red() < c2.red()) return true;
    else if(c1.red() > c2.red()) return false;
    if(c1.green() < c2.green()) return true;
    else if(c1.green() > c2.green()) return false;
    if(c1.blue() < c2.blue()) return true;
    else if(c1.blue() > c2.blue()) return false;
    return false;
}

double ns(double a, double b)
{
    double c = a -b;
    return c*c;
}

struct Genom {
    std::vector<QColor> palette;
    double error;
    void computeError( const std::map<QColor, int>& colors2int )
    {
        this->error = 0.0;
        for(std::map<QColor, int>::const_iterator it = colors2int.begin();
            it != colors2int.end(); ++it)
        {
            double bestScore = -1.0;
            // Find the best color in the palette
            for( std::vector<QColor>::iterator it2 = palette.begin();
                 it2 != palette.end(); ++it2)
            {
                double score = ns(
                                  it->first.red(), it2->red())
                                + ns(it->first.green(), it2->green() )
                                + ns(it->first.blue(), it2->blue() );
                if(score < bestScore or bestScore < 0.0)
                {
                    bestScore = score;
                }
            }
            this->error += sqrt(bestScore) * it->second;
        }
    }
};

inline double randf()
{
    return rand() / (double) RAND_MAX;
}

void mutate(Genom& g)
{
    int index = (int)( randf() * g.palette.size());
    QColor c = g.palette[index];
    c.setRgb( (0.5 - randf()) * 10 + c.red(), (0.5 - randf()) * 10 + c.green(), (0.5 - randf()) * 10 + c.blue() );
    g.palette[index] = c;
}

std::vector<QColor> optimizeColors( const std::map<QColor, int>& colors2int, int paletteSize )
{
    // Sort the colors, with luck it will help the genetic algorithm to eliminate very bad palette early
    std::multimap<int, QColor> int2colors;
    for( std::map<QColor, int>::const_iterator it = colors2int.begin();
            it != colors2int.end(); ++it)
    {
        int2colors.insert( std::multimap<int, QColor>::value_type(-it->second, it->first) );
    }
    // Init the genom
    kdDebug() << "Initialize the genom" << endl;
    std::multimap<double, Genom> genoms;
    for( std::multimap<int, QColor>::iterator it = int2colors.begin();
         it != int2colors.end() ; )
    {
        Genom g;
        while( g.palette.size() < (uint)paletteSize )
        {
            g.palette.push_back( it->second );
            if( it != int2colors.end()) ++it;
        }
        g.computeError(colors2int);
        genoms.insert( std::multimap<double, Genom>::value_type( g.error, g) );
        kdDebug() << g.error << " " << genoms.size() << " out of " << (colors2int.size() / paletteSize) << endl;
    }
    if( genoms.size() & 1 )
    { // Ensure the parity, as we kill half of the genoms
        genoms.insert(  *genoms.begin() );
    }
    double currentBest = genoms.begin()->first;
    int iter = 0;
    for(int iter2 = 0; iter2 < 10; iter++, iter2++)
    {
        kdDebug() << "Iteration : " << iter << endl;
        // Reproduction
        int nbgenoms = 2 * genoms.size();
        std::vector<Genom>parents;
        for(std::multimap<double, Genom>::iterator it = genoms.begin(); it != genoms.end(); ++it)
        {
            parents.push_back( it->second );
        }
        while(genoms.size() <  nbgenoms)
        {
            Genom p1 = parents[(int)(parents.size() * (rand() / (double)RAND_MAX))];
            Genom p2 = parents[(int)(parents.size() * (rand() / (double)RAND_MAX))];
            Genom c1, c2;
            int i = 0;
            int middle = (int)(paletteSize * (rand() / (double)RAND_MAX));
            for(; i < middle; i++)
            {
                c1.palette.push_back( p1.palette[i] );
                c2.palette.push_back( p2.palette[i] );
            }
            for(; i < paletteSize; i++)
            {
                c1.palette.push_back( p2.palette[i] );
                c2.palette.push_back( p1.palette[i] );
            }
            if( rand() >( RAND_MAX)/2) mutate(c1);
            if( rand() <( RAND_MAX)/2) mutate(c2);
                
            c1.computeError(colors2int);
            c2.computeError(colors2int);
            genoms.insert( std::multimap<double, Genom>::value_type( c1.error, c1) );
            genoms.insert( std::multimap<double, Genom>::value_type( c2.error, c2) );
        }
        // Kill the bad genoms
        std::multimap<double, Genom> newGenoms;
        std::multimap<double, Genom>::iterator it = genoms.begin();
        for(int i = 0; i < genoms.size() / 2; ++i, ++it)
        {
            newGenoms.insert( *it );
        }
        genoms = newGenoms;
        kdDebug() << "Best shoot : " << genoms.begin()->first << endl;
        if( currentBest > genoms.begin()->first)
        {
            currentBest = genoms.begin()->first;
            iter2 = 0;
        }
    }
    
    kdDebug() << "Optimization is finished" << endl;
    kdDebug() << genoms.begin()->first << endl;
    return genoms.begin()->second.palette;
}

void generateOptimizedPalette(quint8** colorPalette, int reduction, KisPaintDeviceSP src, const QRect& rect, int paletteSize)
{
    KisColorSpace * cs = src->colorSpace();
    Q_INT32 pixelSize = cs->pixelSize();
    kdDebug() << "Optimization " << reduction << endl;
    QColor c;
    std::map<QColor, int> colors2int;
    KisRectIteratorPixel rectIt = src->createRectIterator(rect.x(), rect.y(), rect.width(), rect.height(), false);
    while(not rectIt.isDone())
    {
        cs->toQColor( rectIt.oldRawData(), &c, (KisProfile*)0 );
        c.setRgb( c.red() >> reduction, c.green() >> reduction, c.blue() >> reduction );
        colors2int[ c ] += 1;
        ++rectIt;
    }
    std::map<QColor, int> colors2intBis;
    for( std::map<QColor, int>::iterator it = colors2int.begin();
            it != colors2int.end(); ++it)
    {
        QColor c = it->first;
        c.setRgb( c.red() << reduction, c.green() << reduction, c.blue() << reduction );
        colors2intBis[c] = it->second;
    }
    std::vector<QColor> colors = optimizeColors( colors2intBis, paletteSize );
    
    for(int i = 0; i < paletteSize; i++)
    {
        quint8* color = new quint8[ pixelSize ];
        cs->fromQColor( colors[i], color, 0 );
        colorPalette[i] = color;
    }
}

void KisDitherFilter::process(KisPaintDeviceSP src, KisPaintDeviceSP dst, 
                                   KisFilterConfiguration* config, const QRect& rect ) 
{
    Q_ASSERT(src != 0);
    Q_ASSERT(dst != 0);
    
    // Dither analysis
    KisColorSpace * cs = src->colorSpace();
    Q_INT32 pixelSize = cs->pixelSize();
    
    QVariant value;
    int paletteSize = 16;
    if (config->getProperty("paletteSize", value))
    {
        paletteSize = value.toInt(0);
    }
    int paletteType = 0;
    if (config->getProperty("paletteType", value))
    {
        paletteType = value.toInt(0);
    }
    quint8** colorPalette = new quint8*[paletteSize];
    switch(paletteType)
    {
        default:
        case 0:
        {
           generateOptimizedPalette(colorPalette, 4, src, rect, paletteSize);
           break;
        }
        case 1:
        {
           generateOptimizedPalette(colorPalette, 2, src, rect, paletteSize);
           break;
        }
        case 2:
        {
           generateOptimizedPalette(colorPalette, 0, src, rect, paletteSize);
           break;
        }
        case 3:
        {
            kdDebug() << "Best colors (4bit)" << endl;
            QColor c;
            std::map<QColor, int> colors2int;
            KisRectIteratorPixel rectIt = src->createRectIterator(rect.x(), rect.y(), rect.width(), rect.height(), false);
            while(not rectIt.isDone())
            {
                cs->toQColor( rectIt.oldRawData(), &c, (KisProfile*)0 );
                colors2int[ c ] += 1;
                ++rectIt;
            }
            std::multimap<int, QColor> int2colors;
            for( std::map<QColor, int>::iterator it = colors2int.begin();
                 it != colors2int.end(); ++it)
            {
                int2colors.insert( std::multimap<int, QColor>::value_type(-it->second, it->first) );
            }
            int realPaletteSize = 0;
            for( std::multimap<int , QColor>::iterator it = int2colors.begin();
                 it != int2colors.end() and realPaletteSize < paletteSize; ++it, ++realPaletteSize)
            {
                quint8* color = new quint8[ pixelSize ];
                cs->fromQColor( it->second, color, 0 );
                colorPalette[realPaletteSize] = color;
            }
            paletteSize = realPaletteSize;
            break;
        }
        case 4:
        {
            kdDebug() << "Most colors (4bit)" << endl;
            QColor c;
            std::map<QColor, int> colors2int;
            KisRectIteratorPixel rectIt = src->createRectIterator(rect.x(), rect.y(), rect.width(), rect.height(), false);
            while(not rectIt.isDone())
            {
                cs->toQColor( rectIt.oldRawData(), &c, (KisProfile*)0 );
                c.setRgb( c.red() >> 4, c.green() >> 4, c.blue() >> 4 );
                colors2int[ c ] += 1;
                ++rectIt;
            }
            std::multimap<int, QColor> int2colors;
            for( std::map<QColor, int>::iterator it = colors2int.begin();
                 it != colors2int.end(); ++it)
            {
                int2colors.insert( std::multimap<int, QColor>::value_type(-it->second, it->first) );
            }
            int realPaletteSize = 0;
            for( std::multimap<int , QColor>::iterator it = int2colors.begin();
                 it != int2colors.end() and realPaletteSize < paletteSize; ++it, ++realPaletteSize)
            {
                quint8* color = new quint8[ pixelSize ];
                QColor c( it->second.red() << 4, it->second.green() << 4, it->second.blue() << 4 );
                cs->fromQColor( c, color, 0 );
                colorPalette[realPaletteSize] = color;
            }
            paletteSize = realPaletteSize;
            break;
        }
        case 5:
            kdDebug() << "Random" << endl;
            for(int i = 0; i < paletteSize; i++)
            {
                QColor c( (int)(rand() * 255.0 / RAND_MAX), (int)(rand() * 255.0 / RAND_MAX), (int)(rand() * 255.0 / RAND_MAX) );
                quint8* color = new quint8[ pixelSize ];
                cs->fromQColor( c, color, 0 );
                colorPalette[i] = color;
            }
            break;
    }
    
    // Apply palette
    KisHLineIteratorPixel dstIt = dst->createHLineIterator(rect.x(), rect.y(), rect.width(), true );
    KisHLineIteratorPixel srcIt = src->createHLineIterator(rect.x(), rect.y(), rect.width(), false);
    int pixelsProcessed = 0;
    setProgressTotalSteps(rect.width() * rect.height());
    
    for(int y = 0; y < rect.height(); y++)
    {
        while( not srcIt.isDone() )
        {
            if(srcIt.isSelected())
            {
                quint8* bestColor = 0;
                double bestDifference = 0.0;
                const quint8* rawData = srcIt.oldRawData();
                for(int i = 0; i < paletteSize; i++)
                {
                    quint8* color = colorPalette[i];
#if 0
                    double delta = cs->difference( color, rawData );
#endif
#if 1
                    double delta = 0.0;
                    for(int j = 0; j < pixelSize; j++)
                    {
                        double a = (color[j] - rawData[j]);
                        delta += a * a;
                    }
#endif
                    if(delta < bestDifference or not bestColor)
                    {
                        bestDifference = delta;
                        bestColor = color;
                    }
                }
                Q_ASSERT(bestColor);
                memcpy( dstIt.rawData(), bestColor, pixelSize);
            }
            setProgress(++pixelsProcessed);
            ++srcIt;
            ++dstIt;
        }
        srcIt.nextRow();
        dstIt.nextRow();
    }

    // Delete palette
    for(int i = 0; i < paletteSize; i++)
    {
      delete[] colorPalette[i];
    }
    delete[] colorPalette;
    setProgressDone(); // Must be called even if you don't really support progression
}
