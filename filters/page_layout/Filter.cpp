/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C)  Joseph Artsimovich <joseph.artsimovich@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Filter.h"
#include "FilterUiInterface.h"
#include "OptionsWidget.h"
#include "Task.h"
#include "Settings.h"
#include "Params.h"
#include "ProjectPages.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "CacheDrivenTask.h"
#include "OrderByWidthProvider.h"
#include "OrderByHeightProvider.h"
#include "Utils.h"
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <XmlMarshaller.h>
#include <XmlUnmarshaller.h>
#include <DefaultParams.h>
#include <DefaultParamsProvider.h>
#include "CommandLine.h"

namespace page_layout {
    Filter::Filter(intrusive_ptr<ProjectPages> const& pages, PageSelectionAccessor const& page_selection_accessor)
            : m_ptrPages(pages),
              m_ptrSettings(new Settings),
              m_selectedPageOrder(0) {
        if (CommandLine::get().isGui()) {
            m_ptrOptionsWidget.reset(
                    new OptionsWidget(m_ptrSettings, page_selection_accessor)
            );
        }

        typedef PageOrderOption::ProviderPtr ProviderPtr;

        ProviderPtr const default_order;
        ProviderPtr const order_by_width(new OrderByWidthProvider(m_ptrSettings));
        ProviderPtr const order_by_height(new OrderByHeightProvider(m_ptrSettings));
        m_pageOrderOptions.emplace_back(tr("Natural order"), default_order);
        m_pageOrderOptions.emplace_back(tr("Order by increasing width"), order_by_width);
        m_pageOrderOptions.emplace_back(tr("Order by increasing height"), order_by_height);
    }

    Filter::~Filter() = default;

    QString Filter::getName() const {
        return tr("Margins");
    }

    PageView Filter::getView() const {
        return PAGE_VIEW;
    }

    void Filter::selected() {
        m_ptrSettings->removePagesMissingFrom(m_ptrPages->toPageSequence(getView()));
    }

    int Filter::selectedPageOrder() const {
        return m_selectedPageOrder;
    }

    void Filter::selectPageOrder(int option) {
        assert((unsigned) option < m_pageOrderOptions.size());
        m_selectedPageOrder = option;
    }

    std::vector<PageOrderOption>
    Filter::pageOrderOptions() const {
        return m_pageOrderOptions;
    }

    void Filter::performRelinking(AbstractRelinker const& relinker) {
        m_ptrSettings->performRelinking(relinker);
    }

    void Filter::preUpdateUI(FilterUiInterface* ui, PageId const& page_id) {
        Margins const margins_mm(m_ptrSettings->getHardMarginsMM(page_id));
        Alignment const alignment(m_ptrSettings->getPageAlignment(page_id));
        m_ptrOptionsWidget->preUpdateUI(page_id, margins_mm, alignment);
        ui->setOptionsWidget(m_ptrOptionsWidget.get(), ui->KEEP_OWNERSHIP);
    }

    QDomElement Filter::saveSettings(ProjectWriter const& writer, QDomDocument& doc) const {
        using namespace boost::lambda;

        QDomElement filter_el(doc.createElement("page-layout"));

        XmlMarshaller marshaller(doc);
        filter_el.appendChild(marshaller.rectF(m_ptrSettings->getContentRect(), "contentRect"));

        writer.enumPages(
                [&](PageId const& page_id, int numeric_id) {
                    this->writePageSettings(doc, filter_el, page_id, numeric_id);
                }
        );

        return filter_el;
    }

    void
    Filter::writePageSettings(QDomDocument& doc, QDomElement& filter_el, PageId const& page_id, int numeric_id) const {
        std::unique_ptr<Params> const params(m_ptrSettings->getPageParams(page_id));
        if (!params.get()) {
            return;
        }

        QDomElement page_el(doc.createElement("page"));
        page_el.setAttribute("id", numeric_id);
        page_el.appendChild(params->toXml(doc, "params"));

        filter_el.appendChild(page_el);
    }

    void Filter::loadSettings(ProjectReader const& reader, QDomElement const& filters_el) {
        m_ptrSettings->clear();

        QDomElement const filter_el(
                filters_el.namedItem("page-layout").toElement()
        );

        QDomElement const rect_el = filter_el.namedItem("contentRect").toElement();
        if (!rect_el.isNull()) {
            m_ptrSettings->setContentRect(XmlUnmarshaller::rectF(rect_el));
        }

        QString const page_tag_name("page");
        QDomNode node(filter_el.firstChild());
        for (; !node.isNull(); node = node.nextSibling()) {
            if (!node.isElement()) {
                continue;
            }
            if (node.nodeName() != page_tag_name) {
                continue;
            }
            QDomElement const el(node.toElement());

            bool ok = true;
            int const id = el.attribute("id").toInt(&ok);
            if (!ok) {
                continue;
            }

            PageId const page_id(reader.pageId(id));
            if (page_id.isNull()) {
                continue;
            }

            QDomElement const params_el(el.namedItem("params").toElement());
            if (params_el.isNull()) {
                continue;
            }

            Params const params(params_el);
            m_ptrSettings->setPageParams(page_id, params);
        }
    }      // Filter::loadSettings

    void Filter::setContentBox(PageId const& page_id, ImageTransformation const& xform, QRectF const& content_rect) {
        QSizeF const content_size_mm(Utils::calcRectSizeMM(xform, content_rect));
        m_ptrSettings->setContentSizeMM(page_id, content_size_mm);
    }

    void Filter::invalidateContentBox(PageId const& page_id) {
        m_ptrSettings->invalidateContentSize(page_id);
    }

    bool Filter::checkReadyForOutput(ProjectPages const& pages, PageId const* ignore) {
        PageSequence const snapshot(pages.toPageSequence(PAGE_VIEW));

        return m_ptrSettings->checkEverythingDefined(snapshot, ignore);
    }

    intrusive_ptr<Task>
    Filter::createTask(PageId const& page_id,
                       intrusive_ptr<output::Task> const& next_task,
                       bool const batch,
                       bool const debug) {
        return intrusive_ptr<Task>(
                new Task(
                        intrusive_ptr<Filter>(this), next_task,
                        m_ptrSettings, page_id, batch, debug
                )
        );
    }

    intrusive_ptr<CacheDrivenTask>
    Filter::createCacheDrivenTask(intrusive_ptr<output::CacheDrivenTask> const& next_task) {
        return intrusive_ptr<CacheDrivenTask>(
                new CacheDrivenTask(next_task, m_ptrSettings)
        );
    }

    void Filter::loadDefaultSettings(PageId const& page_id) {
        if (!m_ptrSettings->isParamsNull(page_id)) {
            return;
        }
        const DefaultParams defaultParams = DefaultParamsProvider::getInstance()->getParams();
        const DefaultParams::PageLayoutParams& pageLayoutParams = defaultParams.getPageLayoutParams();

        // we need to recalculate the margins later basing on metric units and dpi
        m_ptrSettings->setPageParams(
                page_id,
                Params(Margins(-0.01, -0.01, -0.01, -0.01),
                       QRectF(),
                       QRectF(),
                       QSizeF(),
                       pageLayoutParams.getAlignment(),
                       pageLayoutParams.isAutoMargins()
                )
        );
    }
}  // namespace page_layout