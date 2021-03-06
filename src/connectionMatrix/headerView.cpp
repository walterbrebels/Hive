/*
* Copyright (C) 2017-2019, Emilien Vallot, Christophe Calmejane and other contributors

* This file is part of Hive.

* Hive is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.

* Hive is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.

* You should have received a copy of the GNU Lesser General Public License
* along with Hive.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "connectionMatrix/headerView.hpp"
#include "connectionMatrix/model.hpp"
#include "connectionMatrix/node.hpp"
#include "connectionMatrix/paintHelper.hpp"
#include "avdecc/controllerManager.hpp"
#include "toolkit/material/color.hpp"
#include <QPainter>
#include <QContextMenuEvent>
#include <QMenu>

#include <optional>

#if ENABLE_CONNECTION_MATRIX_DEBUG
#	include <QDebug>
#endif

#ifdef _WIN32
#	pragma warning(disable : 4127) // "conditional expression is constant" due to QVector compilation warning
#endif // _WIN32

namespace connectionMatrix
{
HeaderView::HeaderView(Qt::Orientation orientation, QWidget* parent)
	: QHeaderView{ orientation, parent }
{
	setSectionResizeMode(QHeaderView::Fixed);
	setSectionsClickable(true);

	int const size{ 20 };
	setMaximumSectionSize(size);
	setMinimumSectionSize(size);
	setDefaultSectionSize(size);

	setAttribute(Qt::WA_Hover);

	connect(this, &QHeaderView::sectionClicked, this, &HeaderView::handleSectionClicked);
}

void HeaderView::setAlwaysShowArrowTip(bool const show)
{
	_alwaysShowArrowTip = show;
	update();
}
void HeaderView::setAlwaysShowArrowEnd(bool const show)
{
	_alwaysShowArrowEnd = show;
	update();
}

void HeaderView::setTransposed(bool const isTransposed)
{
	_isTransposed = isTransposed;
	update();
}

void HeaderView::setColor(qt::toolkit::material::color::Name const name)
{
	_colorName = name;
	update();
}

QVector<HeaderView::SectionState> HeaderView::saveSectionState() const
{
	return _sectionState;
}

void HeaderView::restoreSectionState(QVector<SectionState> const& sectionState)
{
	if (!AVDECC_ASSERT_WITH_RET(sectionState.count() == count(), "invalid count"))
	{
		_sectionState = {};
		return;
	}

	_sectionState = sectionState;

	for (auto section = 0; section < count(); ++section)
	{
		updateSectionVisibility(section);
	}
}

void HeaderView::setFilterPattern(QRegExp const& pattern)
{
	_pattern = pattern;
	applyFilterPattern();
}

void HeaderView::expandAll()
{
	for (auto section = 0; section < count(); ++section)
	{
		_sectionState[section].expanded = true;
		_sectionState[section].visible = true;

		updateSectionVisibility(section);
	}

	applyFilterPattern();
}

void HeaderView::collapseAll()
{
	auto* model = static_cast<Model*>(this->model());

	for (auto section = 0; section < count(); ++section)
	{
		auto* node = model->node(section, orientation());

		if (node->type() == Node::Type::Entity)
		{
			_sectionState[section].expanded = false;
			_sectionState[section].visible = true;
		}
		else
		{
			_sectionState[section].expanded = false;
			_sectionState[section].visible = false;
		}

		updateSectionVisibility(section);
	}

	applyFilterPattern();
}

void HeaderView::handleSectionClicked(int logicalIndex)
{
	auto* model = static_cast<Model*>(this->model());
	auto* node = model->node(logicalIndex, orientation());

	if (!AVDECC_ASSERT_WITH_RET(node, "invalid node"))
	{
		return;
	}

	if (node->childrenCount() == 0)
	{
		return;
	}

	// Toggle the section expand state
	auto const expanded = !_sectionState[logicalIndex].expanded;
	_sectionState[logicalIndex].expanded = expanded;

#if ENABLE_CONNECTION_MATRIX_DEBUG
	qDebug() << logicalIndex << "is now" << (expanded ? "expanded" : "collapsed");
#endif

	// Update hierarchy visibility
	auto const update = [=](Node* node)
	{
		auto const section = model->section(node, orientation());

		// Do not affect the current node
		if (section == logicalIndex)
		{
			return;
		}

		if (auto* parent = node->parent())
		{
			auto const parentSection = model->section(parent, orientation());
			_sectionState[section].visible = expanded && _sectionState[parentSection].expanded;

			updateSectionVisibility(section);
		}
	};

	model->accept(node, update, true);
}

void HeaderView::handleSectionInserted(QModelIndex const& /*parent*/, int first, int last)
{
	auto const it = std::next(std::begin(_sectionState), first);
	_sectionState.insert(it, last - first + 1, {});

	for (auto section = first; section <= last; ++section)
	{
		auto* model = static_cast<Model*>(this->model());
		auto* node = model->node(section, orientation());

		if (AVDECC_ASSERT_WITH_RET(node, "Node should not be null"))
		{
			auto expanded = true;
			auto visible = true;

			switch (node->type())
			{
				case Node::Type::RedundantOutput:
				case Node::Type::RedundantInput:
					expanded = false;
					break;
				case Node::Type::RedundantOutputStream:
				case Node::Type::RedundantInputStream:
					visible = false;
					break;
				default:
					break;
			}

			_sectionState[section] = { expanded, visible };
			updateSectionVisibility(section);
		}
	}

#if ENABLE_CONNECTION_MATRIX_DEBUG
	qDebug() << "handleSectionInserted" << _sectionState.count();
#endif
}

void HeaderView::handleSectionRemoved(QModelIndex const& /*parent*/, int first, int last)
{
	_sectionState.remove(first, last - first + 1);

#if ENABLE_CONNECTION_MATRIX_DEBUG
	qDebug() << "handleSectionRemoved" << _sectionState.count();
#endif
}

void HeaderView::handleModelReset()
{
	_sectionState.clear();
}

void HeaderView::updateSectionVisibility(int const logicalIndex)
{
	if (!AVDECC_ASSERT_WITH_RET(logicalIndex >= 0 && logicalIndex < _sectionState.count(), "invalid index"))
	{
		return;
	}

	if (_sectionState[logicalIndex].visible)
	{
		showSection(logicalIndex);
	}
	else
	{
		hideSection(logicalIndex);
	}
}

void HeaderView::applyFilterPattern()
{
	auto* model = static_cast<Model*>(this->model());

	auto const showVisitor = [=](Node* node)
	{
		auto const section = model->section(node, orientation());
		updateSectionVisibility(section); // Conditional update
	};

	auto const hideVisitor = [=](Node* node)
	{
		auto const section = model->section(node, orientation());
		hideSection(section); // Hide section no matter what
	};

	for (auto section = 0; section < count(); ++section)
	{
		auto* node = model->node(section, orientation());
		if (node->type() == Node::Type::Entity)
		{
			auto const matches = !node->name().contains(_pattern);

			if (!matches)
			{
				model->accept(node, showVisitor);
			}
			else
			{
				model->accept(node, hideVisitor);
			}
		}
	}
}

void HeaderView::setModel(QAbstractItemModel* model)
{
	if (this->model())
	{
		disconnect(this->model());
	}

	if (!AVDECC_ASSERT_WITH_RET(dynamic_cast<Model*>(model), "invalid pointer kind"))
	{
		return;
	}

	QHeaderView::setModel(model);

	if (model)
	{
		if (orientation() == Qt::Vertical)
		{
			connect(model, &QAbstractItemModel::rowsInserted, this, &HeaderView::handleSectionInserted);
			connect(model, &QAbstractItemModel::rowsRemoved, this, &HeaderView::handleSectionRemoved);
		}
		else
		{
			connect(model, &QAbstractItemModel::columnsInserted, this, &HeaderView::handleSectionInserted);
			connect(model, &QAbstractItemModel::columnsRemoved, this, &HeaderView::handleSectionRemoved);
		}

		connect(model, &QAbstractItemModel::modelReset, this, &HeaderView::handleModelReset);
	}
}

QSize HeaderView::sizeHint() const
{
	if (orientation() == Qt::Horizontal)
	{
		return { defaultSectionSize(), 200 };
	}
	else
	{
		return { 200, defaultSectionSize() };
	}
}

void HeaderView::paintSection(QPainter* painter, QRect const& rect, int logicalIndex) const
{
	auto* model = static_cast<Model*>(this->model());
	auto* node = model->node(logicalIndex, orientation());

	if (!node)
	{
		return;
	}

	auto backgroundColor = QColor{};
	auto foregroundColor = QColor{};
	auto foregroundErrorColor = QColor{};
	auto nodeLevel{ 0 };

	auto const nodeType = node->type();
	// First pass for Bar Color
	switch (nodeType)
	{
		case Node::Type::Entity:
			backgroundColor = qt::toolkit::material::color::value(_colorName, qt::toolkit::material::color::Shade::Shade900);
			foregroundColor = qt::toolkit::material::color::foregroundValue(_colorName, qt::toolkit::material::color::Shade::Shade900);
			foregroundErrorColor = qt::toolkit::material::color::foregroundErrorColorValue(_colorName, qt::toolkit::material::color::Shade::Shade900);
			break;
		case Node::Type::RedundantInput:
		case Node::Type::RedundantOutput:
		case Node::Type::InputStream:
		case Node::Type::OutputStream:
		case Node::Type::InputChannel:
		case Node::Type::OutputChannel:
			backgroundColor = qt::toolkit::material::color::value(_colorName, qt::toolkit::material::color::Shade::Shade600);
			foregroundColor = qt::toolkit::material::color::foregroundValue(_colorName, qt::toolkit::material::color::Shade::Shade600);
			foregroundErrorColor = qt::toolkit::material::color::foregroundErrorColorValue(_colorName, qt::toolkit::material::color::Shade::Shade600);
			nodeLevel = 1;
			break;
		case Node::Type::RedundantInputStream:
		case Node::Type::RedundantOutputStream:
			backgroundColor = qt::toolkit::material::color::value(_colorName, qt::toolkit::material::color::Shade::Shade300);
			foregroundColor = qt::toolkit::material::color::foregroundValue(_colorName, qt::toolkit::material::color::Shade::Shade300);
			foregroundErrorColor = qt::toolkit::material::color::foregroundErrorColorValue(_colorName, qt::toolkit::material::color::Shade::Shade300);
			nodeLevel = 2;
			break;
		default:
			AVDECC_ASSERT(false, "NodeType not handled");
			return;
	}

	// Second pass for Arrow Color
	auto arrowColor = std::optional<QColor>{ std::nullopt };
	switch (nodeType)
	{
		case Node::Type::RedundantInput:
		{
			auto const state = static_cast<RedundantNode const&>(*node).lockedState();
			if (state == Node::TriState::False)
			{
				arrowColor = foregroundErrorColor;
			}
			else if (state == Node::TriState::True)
			{
				arrowColor = backgroundColor;
			}
			break;
		}
		case Node::Type::InputStream:
		case Node::Type::RedundantInputStream:
		{
			auto const state = static_cast<StreamNode const&>(*node).lockedState();
			if (state == Node::TriState::False)
			{
				arrowColor = foregroundErrorColor;
			}
			else if (state == Node::TriState::True)
			{
				arrowColor = backgroundColor;
			}
			break;
		}
		case Node::Type::RedundantOutput:
		{
			if (static_cast<RedundantNode const&>(*node).isStreaming())
			{
				arrowColor = backgroundColor;
			}
			break;
		}
		case Node::Type::OutputStream:
		case Node::Type::RedundantOutputStream:
		{
			if (static_cast<StreamNode const&>(*node).isStreaming())
			{
				arrowColor = backgroundColor;
			}
			break;
		}
		default:
			break;
	}

	auto isSelected{ false };

	if (orientation() == Qt::Horizontal)
	{
		isSelected = selectionModel()->isColumnSelected(logicalIndex, {});
	}
	else
	{
		isSelected = selectionModel()->isRowSelected(logicalIndex, {});
	}

	if (isSelected)
	{
		backgroundColor = qt::toolkit::material::color::complementaryValue(_colorName, qt::toolkit::material::color::Shade::Shade600);
		foregroundColor = qt::toolkit::material::color::foregroundComplementaryValue(_colorName, qt::toolkit::material::color::Shade::Shade600);
	}

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);

	auto const arrowSize{ 10 };
	auto const arrowOffset{ 20 * nodeLevel };

	// Draw the main background arrow
	painter->fillPath(paintHelper::buildHeaderArrowPath(rect, orientation(), _isTransposed, _alwaysShowArrowTip, _alwaysShowArrowEnd, arrowOffset, arrowSize, 0), backgroundColor);

	// Draw the small arrow, if needed
	if (arrowColor)
	{
		auto path = paintHelper::buildHeaderArrowPath(rect, orientation(), _isTransposed, _alwaysShowArrowTip, _alwaysShowArrowEnd, arrowOffset, arrowSize, 5);
		if (orientation() == Qt::Horizontal)
		{
			path.translate(0, 10);
		}
		else
		{
			path.translate(10, 0);
		}

		painter->fillPath(path, *arrowColor);
	}

	painter->translate(rect.topLeft());

	auto textLeftOffset = 0;
	auto textRightOffset = 0;
	auto r = QRect(0, 0, rect.width(), rect.height());
	if (orientation() == Qt::Horizontal)
	{
		r.setWidth(rect.height());
		r.setHeight(rect.width());

		painter->rotate(-90);
		painter->translate(-r.width(), 0);

		r.translate(arrowOffset, 0);

		textLeftOffset = arrowSize;
		textRightOffset = _isTransposed ? (_alwaysShowArrowEnd ? arrowSize : 0) : (_alwaysShowArrowTip ? arrowSize : 0);
	}
	else
	{
		textLeftOffset = _isTransposed ? (_alwaysShowArrowTip ? arrowSize : 0) : (_alwaysShowArrowEnd ? arrowSize : 0);
		textRightOffset = arrowSize;
	}

	auto const padding{ 2 };
	auto textRect = r.adjusted(padding + textLeftOffset, 0, -(padding + textRightOffset + arrowOffset), 0);

	auto const elidedText = painter->fontMetrics().elidedText(node->name(), Qt::ElideMiddle, textRect.width());

	if (node->isStreamNode() && !static_cast<StreamNode*>(node)->isRunning())
	{
		painter->setPen(foregroundErrorColor);
	}
	else
	{
		painter->setPen(foregroundColor);
	}

	painter->drawText(textRect, Qt::AlignVCenter, elidedText);
	painter->restore();
}

void HeaderView::contextMenuEvent(QContextMenuEvent* event)
{
	auto const logicalIndex = logicalIndexAt(event->pos());
	if (logicalIndex < 0)
	{
		return;
	}

	auto* model = static_cast<Model*>(this->model());
	auto* node = model->node(logicalIndex, orientation());

	if (!AVDECC_ASSERT_WITH_RET(node, "invalid node"))
	{
		return;
	}

	if (node->isStreamNode())
	{
		try
		{
			auto& manager = avdecc::ControllerManager::getInstance();
			auto const entityID = node->entityID();
			if (auto controlledEntity = manager.getControlledEntity(entityID))
			{
				auto const& entityNode = controlledEntity->getEntityNode();
				auto const streamIndex = static_cast<StreamNode*>(node)->streamIndex();

				la::avdecc::controller::model::StreamNode const* streamNode{ nullptr };
				auto const isOutputStream = node->type() == Node::Type::OutputStream || node->type() == Node::Type::RedundantOutputStream;

				if (isOutputStream)
				{
					streamNode = &controlledEntity->getStreamOutputNode(entityNode.dynamicModel->currentConfiguration, streamIndex);
				}
				else if (node->type() == Node::Type::InputStream || node->type() == Node::Type::RedundantInputStream)
				{
					streamNode = &controlledEntity->getStreamInputNode(entityNode.dynamicModel->currentConfiguration, streamIndex);
				}

				if (!AVDECC_ASSERT_WITH_RET(streamNode, "invalid node"))
				{
					return;
				}

				auto addHeaderAction = [](QMenu& menu, QString const& text)
				{
					auto* action = menu.addAction(text);
					auto font = action->font();
					font.setBold(true);
					action->setFont(font);
					action->setEnabled(false);
					return action;
				};

				auto addAction = [](QMenu& menu, QString const& text, bool enabled)
				{
					auto* action = menu.addAction(text);
					action->setEnabled(enabled);
					return action;
				};

				QMenu menu;
				addHeaderAction(menu, "Entity: " + avdecc::helper::smartEntityName(*controlledEntity));
				addHeaderAction(menu, "Stream: " + node->name());

				menu.addSeparator();

				auto const isRunning = static_cast<StreamNode*>(node)->isRunning();
				auto* startStreamingAction = addAction(menu, "Start Streaming", !isRunning);
				auto* stopStreamingAction = addAction(menu, "Stop Streaming", isRunning);

				menu.addSeparator();

				// Release the controlled entity before starting a long operation (menu.exec()
				controlledEntity.reset();

				if (auto* action = menu.exec(event->globalPos()))
				{
					if (action == startStreamingAction)
					{
						if (isOutputStream)
						{
							manager.startStreamOutput(entityID, streamIndex);
						}
						else
						{
							manager.startStreamInput(entityID, streamIndex);
						}
					}
					else if (action == stopStreamingAction)
					{
						if (isOutputStream)
						{
							manager.stopStreamOutput(entityID, streamIndex);
						}
						else
						{
							manager.stopStreamInput(entityID, streamIndex);
						}
					}
				}
			}
		}
		catch (...)
		{
		}
	}
}

void HeaderView::mouseMoveEvent(QMouseEvent* event)
{
	if (orientation() == Qt::Horizontal)
	{
		auto const column = logicalIndexAt(static_cast<QMouseEvent*>(event)->pos());
		selectionModel()->select(model()->index(0, column), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Columns);
	}
	else
	{
		auto const row = logicalIndexAt(static_cast<QMouseEvent*>(event)->pos());
		selectionModel()->select(model()->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	}

	QHeaderView::mouseMoveEvent(event);
}

void HeaderView::mouseDoubleClickEvent(QMouseEvent* event)
{
	// Swallow double clicks and transform them into normal mouse press events
	mousePressEvent(event);
}

void HeaderView::leaveEvent(QEvent* event)
{
	selectionModel()->clearSelection();

	QHeaderView::leaveEvent(event);
}

} // namespace connectionMatrix
