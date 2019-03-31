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

#include "connectionMatrix/model.hpp"
#include "connectionMatrix/node.hpp"
#include "avdecc/controllerManager.hpp"
#include "avdecc/helper.hpp"
#include "toolkit/helper.hpp"

#ifndef ENABLE_AVDECC_FEATURE_REDUNDANCY
#	error "Hive requires Redundancy Feature to be enabled in AVDECC Library"
#endif // ENABLE_AVDECC_FEATURE_REDUNDANCY

namespace connectionMatrix
{

namespace priv
{

using Nodes = std::vector<Node*>;

// Entity node by entity ID
using NodeMap = std::unordered_map<la::avdecc::UniqueIdentifier, std::unique_ptr<Node>, la::avdecc::UniqueIdentifier::hash>;

// Entity section by entity ID
using EntitySectionMap = std::unordered_map<la::avdecc::UniqueIdentifier, int, la::avdecc::UniqueIdentifier::hash>;

// Stream identifier by entity ID and index
using StreamSectionKey = std::pair<la::avdecc::UniqueIdentifier, la::avdecc::entity::model::StreamIndex>;

struct StreamSectionKeyHash
{
	std::size_t operator()(StreamSectionKey const& key) const
	{
		return la::avdecc::UniqueIdentifier::hash()(key.first) ^ std::hash<int>()(key.second);
	}
};

// Stream section by entity ID and index
using StreamSectionMap = std::unordered_map<StreamSectionKey, int, StreamSectionKeyHash>;

// index by node
using NodeSectionMap = std::unordered_map<Node*, int>;

enum class IntersectionDirtyFlag
{
	UpdateConnected = 1u << 0, /**< Update the connected status, or the summary if this is a parent node */
	UpdateFormat = 1u << 1, /**<  Update the matching format status, or the summary if this is a parent node */
	UpdateGptp = 1u << 2, /**< Update the matching gPTP status, or the summary if this is a parent node (WARNING: For intersection of redundant and non-redundant, the complete checks has to be done, since format compatibility is not checked if GM is not the same) */
	UpdateLinkStatus = 1u << 3, /**< Update the link status, or the summary if this is a parent node */
};
using IntersectionDirtyFlags = la::avdecc::utils::EnumBitfield<IntersectionDirtyFlag>;

// Flatten node hierarchy and insert all nodes in list
void insertNodes(std::vector<Node*>& list, Node* node)
{
	if (!node)
	{
		assert(false);
		return;
	}
	
#if ENABLE_CONNECTION_MATRIX_DEBUG
	auto const before = list.size();
#endif
	
	node->accept([&list](Node* node)
	{
		list.push_back(node);
	});
	
#if ENABLE_CONNECTION_MATRIX_DEBUG
	auto const after = list.size();
	qDebug() << "insertNodes" << before << ">" << after;
#endif
}

// Range vector removal
void removeNodes(Nodes& list, int first, int last)
{
	auto const from = std::next(std::begin(list), first);
	auto const to = std::next(std::begin(list), last);
	
	assert(from < to);
	
#if ENABLE_CONNECTION_MATRIX_DEBUG
	auto const before = list.size();
#endif
	
	list.erase(from, to);

#if ENABLE_CONNECTION_MATRIX_DEBUG
	auto const after = list.size();
	qDebug() << "removeNodes" << before << ">" << after;
#endif
}

// Returns the total number of children in node hierarchy
int absoluteChildrenCount(Node* node)
{
	if (!node)
	{
		assert(false);
		return 0;
	}

	auto count = 0;
	
	for (auto i = 0; i < node->childrenCount(); ++i)
	{
		auto* child = node->childAt(i);
		count += 1 + absoluteChildrenCount(child);
	}
	
	return count;
}

// Builds and returns an EntitySectionMap from nodes
EntitySectionMap buildEntitySectionMap(Nodes const& nodes)
{
	EntitySectionMap sectionMap;
	
	for (auto section = 0u; section < nodes.size(); ++section)
	{
		auto* node = nodes[section];
		if (node->type() == Node::Type::Entity)
		{
#if ENABLE_CONNECTION_MATRIX_DEBUG
			qDebug() << "buildEntitySectionMap" << node->name() << "at section" << section;
#endif
		
			auto const [it, result] = sectionMap.insert(std::make_pair(node->entityID(), section));
			
			assert(result);
		}
	}
	
	return sectionMap;
}

// Build and returns a StreamSectionMap from nodes
StreamSectionMap buildStreamSectionMap(Nodes const& nodes)
{
	StreamSectionMap sectionMap;
	
	for (auto section = 0u; section < nodes.size(); ++section)
	{
		auto* node = nodes[section];
		if (node->isStreamNode())
		{
			auto const entityID = node->entityID();
			auto const streamIndex = static_cast<StreamNode*>(node)->streamIndex();
			
#if ENABLE_CONNECTION_MATRIX_DEBUG
			qDebug() << "buildStreamSectionMap" << node->name() << ", stream" << streamIndex << "at section" << section;
#endif
			
			auto const key = std::make_pair(entityID, streamIndex);
			auto const [it, result] = sectionMap.insert(std::make_pair(key, section));
			
			assert(result);
		}
	}
	
	return sectionMap;
}

// Build and returns a NodeSectionMap from nodes
NodeSectionMap buildNodeSectionMap(Nodes const& nodes)
{
	NodeSectionMap sectionMap;
	
	for (auto section = 0u; section < nodes.size(); ++section)
	{
		auto* node = nodes[section];
		sectionMap.insert(std::make_pair(node, section));
	}
	
	return sectionMap;
}

// Return the index of a node contained in a NodeSectionMap
int indexOf(NodeSectionMap const& map, Node* node)
{
	auto const it = map.find(node);
	assert(it != std::end(map));
	return it->second;
}

// Determines intersection type according to talker and listener
Model::IntersectionData::Type determineIntersectionType(Node* talker, Node* listener)
{
	assert(talker);
	assert(listener);

	if (talker->entityID() == listener->entityID())
	{
		return Model::IntersectionData::Type::None;
	}

	auto const talkerType = talker->type();
	auto const listenerType = listener->type();

	if (talkerType == Node::Type::Entity && listenerType == Node::Type::Entity)
	{
		return Model::IntersectionData::Type::Entity_Entity;
	}

	if (talkerType == Node::Type::Entity || listenerType == Node::Type::Entity)
	{
		if (talkerType == Node::Type::RedundantOutput || listenerType == Node::Type::RedundantInput)
		{
			return Model::IntersectionData::Type::Entity_Redundant;
		}

		if (talkerType == Node::Type::RedundantOutputStream || listenerType == Node::Type::RedundantInputStream)
		{
			return Model::IntersectionData::Type::Entity_RedundantStream;
		}

		if (talkerType == Node::Type::OutputStream || listenerType == Node::Type::InputStream)
		{
			return Model::IntersectionData::Type::Entity_SingleStream;
		}
	}

	if (talkerType == Node::Type::RedundantOutput && listenerType == Node::Type::RedundantInput)
	{
		return Model::IntersectionData::Type::Redundant_Redundant;
	}

	if (talkerType == Node::Type::RedundantOutput || listenerType == Node::Type::RedundantInput)
	{
		if (talkerType == Node::Type::RedundantOutputStream || listenerType == Node::Type::RedundantInputStream)
		{
			return Model::IntersectionData::Type::Redundant_RedundantStream;
		}

		if (talkerType == Node::Type::OutputStream || listenerType == Node::Type::InputStream)
		{
			return Model::IntersectionData::Type::Redundant_SingleStream;
		}
	}

	if (talkerType == Node::Type::RedundantOutputStream && listenerType == Node::Type::RedundantInputStream)
	{
		if (talker->index() == listener->index())
		{
			return Model::IntersectionData::Type::RedundantStream_RedundantStream;
		}
		else
		{
			return Model::IntersectionData::Type::None;
		}
	}

	if (talkerType == Node::Type::RedundantOutputStream || listenerType == Node::Type::RedundantInputStream)
	{
		if (talkerType == Node::Type::OutputStream || listenerType == Node::Type::InputStream)
		{
			return Model::IntersectionData::Type::RedundantStream_SingleStream;
		}
	}

	if (talkerType == Node::Type::OutputStream && listenerType == Node::Type::InputStream)
	{
		return Model::IntersectionData::Type::SingleStream_SingleStream;
	}

	assert(false);

	return Model::IntersectionData::Type::None;
}

// Updates intersection data for the given dirtyFlags
void computeIntersectionCapabilities(Model::IntersectionData& intersectionData, IntersectionDirtyFlags const dirtyFlags)
{
	try
	{
		auto& manager = avdecc::ControllerManager::getInstance();
		
		auto const talkerType = intersectionData.talker->type();
		auto const listenerType = intersectionData.listener->type();
		
		auto const talkerEntityID = intersectionData.talker->entityID();
		auto const listenerEntityID = intersectionData.listener->entityID();

		auto talkerEntity = manager.getControlledEntity(talkerEntityID);
		auto listenerEntity = manager.getControlledEntity(listenerEntityID);
		
		auto const& talkerEntityNode = talkerEntity->getEntityNode();
		auto const& listenerEntityNode = listenerEntity->getEntityNode();
		
		auto const talkerConfigurationIndex = talkerEntityNode.dynamicModel->currentConfiguration;
		auto const listenerConfigurationIndex = listenerEntityNode.dynamicModel->currentConfiguration;

		switch (intersectionData.type)
		{
		case Model::IntersectionData::Type::Entity_Entity:
		case Model::IntersectionData::Type::Entity_Redundant:
		case Model::IntersectionData::Type::Entity_RedundantStream:
		case Model::IntersectionData::Type::Entity_SingleStream:
		{
			// At least one entity node: we want to know if at least one connection is established
		}
			break;
		case Model::IntersectionData::Type::Redundant_Redundant:
		{
			// Both redundant nodes: we want to differentiate full redundant connection (both pairs connected) from partial one (only one of the pair connected)
			auto const& talkerRedundantNode = talkerEntity->getRedundantStreamOutputNode(talkerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.talker)->redundantIndex());
			auto const& listenerRedundantNode = listenerEntity->getRedundantStreamInputNode(listenerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.listener)->redundantIndex());
			
			auto atLeastOneInterfaceDown = false;
			auto atLeastOneConnected = false;
			auto allConnected = true;
			auto allSameDomain = true;
			auto allSameFormat = true;
			
			auto const& talkerRedundantStreams = talkerRedundantNode.redundantStreams;
			auto const& listenerRedundantStreams = listenerRedundantNode.redundantStreams;
			assert(talkerRedundantStreams.size() == listenerRedundantStreams.size());
			
			auto it = std::make_pair(std::begin(talkerRedundantStreams), std::begin(talkerRedundantStreams));
			auto const end = std::make_pair(std::end(talkerRedundantStreams), std::end(talkerRedundantStreams));

			// Pair iteration
			for (; it != end; ++it.first, ++it.second)
			{
				auto const* const redundantTalkerStreamNode = static_cast<la::avdecc::controller::model::StreamOutputNode const*>(it.first->second);
				auto const* const redundantListenerStreamNode = static_cast<la::avdecc::controller::model::StreamInputNode const*>(it.second->second);
				
				auto const connected = avdecc::helper::isStreamConnected(talkerEntityID, redundantTalkerStreamNode, redundantListenerStreamNode);
				
				atLeastOneInterfaceDown =
				atLeastOneConnected |= connected;
				
				allConnected &= connected;
			}

			if (atLeastOneInterfaceDown)
			{
				intersectionData.capabilities.set(Model::IntersectionData::Capability::InterfaceDown);
			}
			else
			{
				intersectionData.capabilities.reset(Model::IntersectionData::Capability::InterfaceDown);
			}

			if (atLeastOneConnected)
			{
				if (allConnected)
				{
					intersectionData.capabilities.set(Model::IntersectionData::Capability::Connected);
				}
				else
				{
					// Partially connected
				}
			}
			else
			{
				intersectionData.capabilities.reset(Model::IntersectionData::Capability::Connected);
			}
		}
			break;
		case Model::IntersectionData::Type::Redundant_SingleStream:
		{
			// Redundant node and non-redundant stream: We want to check if one connection is active or possible (only one should be, a non-redundant device can only be connected with either of the redundant domain pair)
			auto redundantConfigurationIndex = la::avdecc::entity::model::getInvalidDescriptorIndex();
			la::avdecc::controller::ControlledEntity const* redundantEntity{ nullptr };
			la::avdecc::controller::model::RedundantStreamNode const* redundantStreamNode{ nullptr };
			la::avdecc::controller::model::StreamNode const* nonRedundantStreamNode{ nullptr };
			la::avdecc::controller::model::AvbInterfaceNode const* nonRedundantAvbInterfaceNode{ nullptr };

			// Determine the redundant and non-redundant nodes
			if (talkerType == Node::Type::RedundantOutput)
			{
				redundantConfigurationIndex = talkerConfigurationIndex;
				redundantEntity = talkerEntity.get();
				redundantStreamNode = &talkerEntity->getRedundantStreamOutputNode(talkerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.talker)->redundantIndex());
				nonRedundantStreamNode = &listenerEntity->getStreamInputNode(listenerConfigurationIndex, static_cast<StreamNode*>(intersectionData.listener)->streamIndex());
				if (nonRedundantStreamNode->staticModel)
				{
					nonRedundantAvbInterfaceNode = &listenerEntity->getAvbInterfaceNode(listenerConfigurationIndex, static_cast<StreamNode*>(intersectionData.listener)->avbInterfaceIndex());
				}
			}
			else if (listenerType == Node::Type::RedundantInput)
			{
				redundantConfigurationIndex = listenerConfigurationIndex;
				redundantEntity = listenerEntity.get();
				redundantStreamNode = &listenerEntity->getRedundantStreamInputNode(listenerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.listener)->redundantIndex());
				nonRedundantStreamNode = &talkerEntity->getStreamOutputNode(talkerConfigurationIndex, static_cast<StreamNode*>(intersectionData.talker)->streamIndex());
				if (nonRedundantStreamNode->staticModel)
				{
					nonRedundantAvbInterfaceNode = &talkerEntity->getAvbInterfaceNode(talkerConfigurationIndex, static_cast<StreamNode*>(intersectionData.talker)->avbInterfaceIndex());
				}
			}
			else
			{
				assert(false);
			}

			// Try to find if an interface of the redundant device is connected to the same domain that the non-redundant device
			auto matchingRedundantStreamIndex = la::avdecc::entity::model::StreamIndex{ la::avdecc::entity::model::getInvalidDescriptorIndex() };
			auto nonRedundantGrandmasterID = (nonRedundantAvbInterfaceNode && nonRedundantAvbInterfaceNode->dynamicModel) ? nonRedundantAvbInterfaceNode->dynamicModel->avbInfo.gptpGrandmasterID : la::avdecc::UniqueIdentifier::getNullUniqueIdentifier();

			for (auto const& [redundantStreamIndex, redundantStreamNode] : redundantStreamNode->redundantStreams)
			{
				if (redundantStreamNode->staticModel)
				{
					auto const& redundantAvbInterfaceNode = redundantEntity->getAvbInterfaceNode(redundantConfigurationIndex, redundantStreamNode->staticModel->avbInterfaceIndex);
					if (redundantAvbInterfaceNode.dynamicModel && redundantAvbInterfaceNode.dynamicModel->avbInfo.gptpGrandmasterID == nonRedundantGrandmasterID)
					{
						matchingRedundantStreamIndex = redundantStreamIndex;
						break;
					}
				}
			}

			auto areMatchingDomainsConnected = false;
			auto areMatchingDomainsFastConnecting = false;
			auto isFormatCompatible = true;

			auto const foundMatchingRedundantStreamIndex = matchingRedundantStreamIndex != la::avdecc::entity::model::getInvalidDescriptorIndex();

			// Found a matching domain
			if (foundMatchingRedundantStreamIndex)
			{
				// Get format compatibility and connection state
				if (talkerType == Node::Type::RedundantOutput)
				{
					auto const& talkerStreamNode = redundantEntity->getStreamOutputNode(redundantConfigurationIndex, matchingRedundantStreamIndex);

					areMatchingDomainsConnected = avdecc::helper::isStreamConnected(talkerEntityID, &talkerStreamNode, static_cast<la::avdecc::controller::model::StreamInputNode const*>(nonRedundantStreamNode));
					areMatchingDomainsFastConnecting = avdecc::helper::isStreamFastConnecting(talkerEntityID, &talkerStreamNode, static_cast<la::avdecc::controller::model::StreamInputNode const*>(nonRedundantStreamNode));

					auto const talkerStreamFormat = talkerStreamNode.dynamicModel->streamInfo.streamFormat;
					auto const listenerStreamFormat = static_cast<la::avdecc::controller::model::StreamInputNode const*>(nonRedundantStreamNode)->dynamicModel->streamInfo.streamFormat;

					isFormatCompatible = la::avdecc::entity::model::StreamFormatInfo::isListenerFormatCompatibleWithTalkerFormat(listenerStreamFormat, talkerStreamFormat);
				}
				else if (listenerType == Node::Type::RedundantInput)
				{
					auto const& listenerStreamNode = redundantEntity->getStreamInputNode(redundantConfigurationIndex, matchingRedundantStreamIndex);

					areMatchingDomainsConnected = avdecc::helper::isStreamConnected(talkerEntityID, static_cast<la::avdecc::controller::model::StreamOutputNode const*>(nonRedundantStreamNode), &listenerStreamNode);
					areMatchingDomainsFastConnecting = avdecc::helper::isStreamFastConnecting(talkerEntityID, static_cast<la::avdecc::controller::model::StreamOutputNode const*>(nonRedundantStreamNode), &listenerStreamNode);

					auto const talkerStreamFormat = static_cast<la::avdecc::controller::model::StreamOutputNode const*>(nonRedundantStreamNode)->dynamicModel->streamInfo.streamFormat;
					auto const listenerStreamFormat = listenerStreamNode.dynamicModel->streamInfo.streamFormat;

					isFormatCompatible = la::avdecc::entity::model::StreamFormatInfo::isListenerFormatCompatibleWithTalkerFormat(listenerStreamFormat, talkerStreamFormat);
				}
				else
				{
					assert(false);
				}
			}

			auto areConnected = areMatchingDomainsConnected;
			auto fastConnecting = areMatchingDomainsFastConnecting;

			// Always check for all connection
			for (auto const& redundantStreamKV : redundantStreamNode->redundantStreams)
			{
				if (talkerType == Node::Type::RedundantOutput)
				{
					auto const* const talkerStreamNode = static_cast<la::avdecc::controller::model::StreamOutputNode const*>(redundantStreamKV.second);

					areConnected |= avdecc::helper::isStreamConnected(talkerEntityID, talkerStreamNode, static_cast<la::avdecc::controller::model::StreamInputNode const*>(nonRedundantStreamNode));
					fastConnecting |= avdecc::helper::isStreamFastConnecting(talkerEntityID, talkerStreamNode, static_cast<la::avdecc::controller::model::StreamInputNode const*>(nonRedundantStreamNode));
				}
				else if (listenerType == Node::Type::RedundantInput)
				{
					auto const* const listenerStreamNode = static_cast<la::avdecc::controller::model::StreamInputNode const*>(redundantStreamKV.second);

					areConnected |= avdecc::helper::isStreamConnected(talkerEntityID, static_cast<la::avdecc::controller::model::StreamOutputNode const*>(nonRedundantStreamNode), listenerStreamNode);
					fastConnecting |= avdecc::helper::isStreamFastConnecting(talkerEntityID, static_cast<la::avdecc::controller::model::StreamOutputNode const*>(nonRedundantStreamNode), listenerStreamNode);
				}
				else
				{
					assert(false);
				}
			}

			// Update connected state
			if (areConnected)
			{
				intersectionData.capabilities.set(Model::IntersectionData::Capability::Connected);
			}
			else
			{
				intersectionData.capabilities.reset(Model::IntersectionData::Capability::Connected);
			}

			if (fastConnecting)
			{
				intersectionData.capabilities.set(Model::IntersectionData::Capability::FastConnecting);
			}
			else
			{
				intersectionData.capabilities.reset(Model::IntersectionData::Capability::FastConnecting);
			}

			// Set domain as compatible if there is a valid matching domain AND either no connection at all OR matching domain connection
			if (foundMatchingRedundantStreamIndex)
			{
				intersectionData.capabilities.reset(Model::IntersectionData::Capability::WrongDomain);
			}
			else
			{
				intersectionData.capabilities.set(Model::IntersectionData::Capability::WrongDomain);
			}
		}
			break;
		case Model::IntersectionData::Type::RedundantStream_RedundantStream:
		case Model::IntersectionData::Type::RedundantStream_SingleStream:
		case Model::IntersectionData::Type::SingleStream_SingleStream:
		{
			// All other cases
			la::avdecc::controller::model::StreamOutputNode const* talkerStreamNode{ nullptr };
			
			if (talkerType == Node::Type::RedundantOutput)
			{
				auto const& redundantNode = talkerEntity->getRedundantStreamOutputNode(talkerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.talker)->redundantIndex());
				auto const listenerRedundantStreamOrder = static_cast<std::size_t>(intersectionData.listener->index());
				assert(listenerRedundantStreamOrder < redundantNode.redundantStreams.size() && "Invalid redundant stream index");
				auto it = std::begin(redundantNode.redundantStreams);
				std::advance(it, listenerRedundantStreamOrder);
				talkerStreamNode = static_cast<la::avdecc::controller::model::StreamOutputNode const*>(it->second);
				assert(talkerStreamNode->isRedundant);
			}
			else if (talkerType == Node::Type::OutputStream)
			{
				talkerStreamNode = &talkerEntity->getStreamOutputNode(talkerConfigurationIndex, static_cast<StreamNode*>(intersectionData.talker)->streamIndex());
			}
			else
			{
				//assert(false);
			}
			
			la::avdecc::controller::model::StreamInputNode const* listenerStreamNode{ nullptr };
			
			if (listenerType == Node::Type::RedundantInput)
			{
				auto const& redundantNode = listenerEntity->getRedundantStreamInputNode(listenerConfigurationIndex, static_cast<RedundantNode*>(intersectionData.listener)->redundantIndex());
				auto const talkerRedundantStreamOrder = static_cast<std::size_t>(intersectionData.listener->index());
				assert(talkerRedundantStreamOrder < redundantNode.redundantStreams.size() && "Invalid redundant stream index");
				auto it = std::begin(redundantNode.redundantStreams);
				std::advance(it, talkerRedundantStreamOrder);
				listenerStreamNode = static_cast<la::avdecc::controller::model::StreamInputNode const*>(it->second);
				assert(listenerStreamNode->isRedundant);
			}
			else if (listenerType == Node::Type::InputStream)
			{
				listenerStreamNode = &listenerEntity->getStreamInputNode(listenerConfigurationIndex, static_cast<StreamNode*>(intersectionData.listener)->streamIndex());
			}
			else
			{
				//assert(false);
			}

			if (!talkerStreamNode || !listenerStreamNode)
			{
				return;
			}
			
			// TODO, filter using dirty flags
			
			{
				auto const talkerAvbInterfaceIndex = static_cast<StreamNode*>(intersectionData.talker)->avbInterfaceIndex();
				auto const listenerAvbInterfaceIndex = static_cast<StreamNode*>(intersectionData.listener)->avbInterfaceIndex();
				
				auto const talkerInterfaceLinkStatus = talkerEntity->getAvbInterfaceLinkStatus(talkerAvbInterfaceIndex);
				auto const listenerInterfaceLinkStatus = listenerEntity->getAvbInterfaceLinkStatus(listenerAvbInterfaceIndex);

				auto const interfaceDown = talkerInterfaceLinkStatus == la::avdecc::controller::ControlledEntity::InterfaceLinkStatus::Down || listenerInterfaceLinkStatus == la::avdecc::controller::ControlledEntity::InterfaceLinkStatus::Down;

				// InterfaceDown
				{
					if (interfaceDown)
					{
						intersectionData.capabilities.set(Model::IntersectionData::Capability::InterfaceDown);
					}
					else
					{
						intersectionData.capabilities.reset(Model::IntersectionData::Capability::InterfaceDown);
					}
				}
				
				// SameDomain
				{
					auto wrongDomain = true;
					
					if (interfaceDown)
					{
						wrongDomain = true;
					}
					else
					{
						auto const& talkerAvbInterfaceNode = talkerEntity->getAvbInterfaceNode(talkerConfigurationIndex, talkerAvbInterfaceIndex);
						auto const& listenerAvbInterfaceNode = listenerEntity->getAvbInterfaceNode(listenerConfigurationIndex, listenerAvbInterfaceIndex);
						
						auto const& talkerAvbInfo = talkerAvbInterfaceNode.dynamicModel->avbInfo;
						auto const& listenerAvbInfo = listenerAvbInterfaceNode.dynamicModel->avbInfo;
						
						wrongDomain = talkerAvbInfo.gptpGrandmasterID != listenerAvbInfo.gptpGrandmasterID;
					}
				
					if (wrongDomain)
					{
						intersectionData.capabilities.set(Model::IntersectionData::Capability::WrongDomain);
					}
					else
					{
						intersectionData.capabilities.reset(Model::IntersectionData::Capability::WrongDomain);
					}
				}
			}
			
			// Connected
			{
				if (avdecc::helper::isStreamConnected(talkerEntityID, talkerStreamNode, listenerStreamNode))
				{
					intersectionData.capabilities.set(Model::IntersectionData::Capability::Connected);
				}
				else
				{
					intersectionData.capabilities.reset(Model::IntersectionData::Capability::Connected);
				}
			}
			
			// SameFormat
			{
				auto const talkerStreamFormat = talkerStreamNode->dynamicModel->streamInfo.streamFormat;
				auto const listenerStreamFormat = listenerStreamNode->dynamicModel->streamInfo.streamFormat;
				
				if (!la::avdecc::entity::model::StreamFormatInfo::isListenerFormatCompatibleWithTalkerFormat(listenerStreamFormat, talkerStreamFormat))
				{
					intersectionData.capabilities.set(Model::IntersectionData::Capability::WrongFormat);
				}
				else
				{
					intersectionData.capabilities.reset(Model::IntersectionData::Capability::WrongFormat);
				}
			}
		}
		default:
			break;
		}
	}
	catch (...)
	{
		//
	}
}


// Initializes static intersection data
void initializeIntersectionData(Node* talker, Node* listener, Model::IntersectionData& intersectionData)
{
	assert(talker);
	assert(listener);

	intersectionData.type = determineIntersectionType(talker, listener);
	intersectionData.talker = talker;
	intersectionData.listener = listener;
	intersectionData.capabilities = {};
	
	IntersectionDirtyFlags dirtyFlags;
	dirtyFlags.assign(0xffff); // Compute everything for initial state
	
	computeIntersectionCapabilities(intersectionData, dirtyFlags);
}

} // namespace priv

class ModelPrivate : public QObject
{
	Q_OBJECT
public:
	ModelPrivate(Model* q)
	: q_ptr{ q }
	{
		auto& controllerManager = avdecc::ControllerManager::getInstance();
		connect(&controllerManager, &avdecc::ControllerManager::controllerOffline, this, &ModelPrivate::handleControllerOffline);
		connect(&controllerManager, &avdecc::ControllerManager::entityOnline, this, &ModelPrivate::handleEntityOnline);
		connect(&controllerManager, &avdecc::ControllerManager::entityOffline, this, &ModelPrivate::handleEntityOffline);
		connect(&controllerManager, &avdecc::ControllerManager::streamRunningChanged, this, &ModelPrivate::handleStreamRunningChanged);
		connect(&controllerManager, &avdecc::ControllerManager::streamConnectionChanged, this, &ModelPrivate::handleStreamConnectionChanged);
		connect(&controllerManager, &avdecc::ControllerManager::streamFormatChanged, this, &ModelPrivate::handleStreamFormatChanged);
		connect(&controllerManager, &avdecc::ControllerManager::gptpChanged, this, &ModelPrivate::handleGptpChanged);
		connect(&controllerManager, &avdecc::ControllerManager::entityNameChanged, this, &ModelPrivate::handleEntityNameChanged);
		connect(&controllerManager, &avdecc::ControllerManager::streamNameChanged, this, &ModelPrivate::handleStreamNameChanged);
		connect(&controllerManager, &avdecc::ControllerManager::avbInterfaceLinkStatusChanged, this, &ModelPrivate::handleAvbInterfaceLinkStatusChanged);
	}

#if ENABLE_CONNECTION_MATRIX_DEBUG
	void dump() const
	{
		auto const rows = _intersectionData.size();
		auto const columns = rows > 0 ? _intersectionData.at(0).size() : 0u;
			
		qDebug() << "talkers" << _talkerNodes.size();
		qDebug() << "listeners" << _listenerNodes.size();
		qDebug() << "capabilities" << rows << "x" << columns;
	}
#endif

#if ENABLE_CONNECTION_MATRIX_DEBUG
	void highlightIntersection(int talkerSection, int listenerSection)
	{
		assert(isValidTalkerSection(talkerSection));
		assert(isValidListenerSection(listenerSection));

		auto& intersectionData = _intersectionData.at(talkerSection).at(listenerSection);

		if (!intersectionData.animation)
		{
			intersectionData.animation = new QVariantAnimation{ this };
		}

		intersectionData.animation->setStartValue(QColor{ Qt::red });
		intersectionData.animation->setEndValue(QColor{ Qt::transparent });
		intersectionData.animation->setDuration(500);
		intersectionData.animation->start();

		connect(intersectionData.animation, &QVariantAnimation::valueChanged, [this, talkerSection, listenerSection](QVariant const& value)
		{
			Q_Q(Model);
			auto const index = q->index(talkerSection, listenerSection);
			emit q->dataChanged(index, index);
		});
	}
#endif

	// Notification wrappers
	
	void beginInsertTalkerItems(int first, int last)
	{
		Q_Q(Model);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "beginInsertTalkerItems(" << first << "," << last << ")";
#endif
	
		if (!_transposed)
		{
			emit q->beginInsertRows({}, first, last);
		}
		else
		{
			emit q->beginInsertColumns({}, first, last);
		}
	}
	
	void endInsertTalkerItems()
	{
		Q_Q(Model);

		if (!_transposed)
		{
			emit q->endInsertRows();
		}
		else
		{
			emit q->endInsertColumns();
		}
	}
	
	void beginRemoveTalkerItems(int first, int last)
	{
		Q_Q(Model);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "beginRemoveTalkerItems(" << first << "," << last << ")";
#endif

		if (!_transposed)
		{
			emit q->beginRemoveRows({}, first, last);
		}
		else
		{
			emit q->beginRemoveColumns({}, first, last);
		}
	}
	
	void endRemoveTalkerItems()
	{
		Q_Q(Model);

		if (!_transposed)
		{
			emit q->endRemoveRows();
		}
		else
		{
			emit q->endRemoveColumns();
		}
	}
	
	void beginInsertListenerItems(int first, int last)
	{
		Q_Q(Model);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "beginInsertListenerItems(" << first << "," << last << ")";
#endif

		if (!_transposed)
		{
			emit q->beginInsertColumns({}, first, last);
		}
		else
		{
			emit q->beginInsertRows({}, first, last);
		}
	}
	
	void endInsertListenerItems()
	{
		Q_Q(Model);

		if (!_transposed)
		{
			emit q->endInsertColumns();
		}
		else
		{
			emit q->endInsertRows();
		}
	}
	
	void beginRemoveListenerItems(int first, int last)
	{
		Q_Q(Model);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "beginRemoveListenerItems(" << first << "," << last << ")";
#endif

		if (!_transposed)
		{
			emit q->beginRemoveColumns({}, first, last);
		}
		else
		{
			emit q->beginRemoveRows({}, first, last);
		}
	}
	
	void endRemoveListenerItems()
	{
		Q_Q(Model);

		if (!_transposed)
		{
			emit q->endRemoveColumns();
		}
		else
		{
			emit q->endRemoveRows();
		}
	}
	
	// Insertion / removal helpers
	
	void rebuildTalkerSectionCache()
	{
		_talkerStreamSectionMap = priv::buildStreamSectionMap(_talkerNodes);
		_talkerNodeSectionMap = priv::buildNodeSectionMap(_talkerNodes);
	}
	
	void rebuildListenerSectionCache()
	{
		_listenerStreamSectionMap = priv::buildStreamSectionMap(_listenerNodes);
		_listenerNodeSectionMap = priv::buildNodeSectionMap(_listenerNodes);
	}
	
	Node* buildTalkerNode(la::avdecc::controller::ControlledEntity const& controlledEntity, la::avdecc::UniqueIdentifier const& entityID, la::avdecc::controller::model::ConfigurationNode const& configurationNode)
	{
		try
		{
			auto const& entityNode = controlledEntity.getEntityNode();
			auto const currentConfigurationIndex = entityNode.dynamicModel->currentConfiguration;

			auto* entity = EntityNode::create(entityID);
			entity->setName(avdecc::helper::smartEntityName(controlledEntity));

			// Redundant streams
			for (auto const&[redundantIndex, redundantNode] : configurationNode.redundantStreamOutputs)
			{
				auto* redundantOutput = RedundantNode::createOutputNode(*entity, redundantIndex);
				redundantOutput->setName(avdecc::helper::redundantOutputName(redundantIndex));

				auto const& redundantStreamNode = controlledEntity.getRedundantStreamOutputNode(currentConfigurationIndex, redundantIndex);

				for (auto const&[streamIndex, streamNode] : redundantNode.redundantStreams)
				{
					auto const avbInterfaceIndex{ streamNode->staticModel->avbInterfaceIndex };
					auto const& avbInterfaceNode = controlledEntity.getAvbInterfaceNode(currentConfigurationIndex, avbInterfaceIndex);

					auto* redundantOutputStream = StreamNode::createRedundantOutputNode(*redundantOutput, streamIndex, avbInterfaceIndex);
					redundantOutputStream->setName(avdecc::helper::outputStreamName(controlledEntity, streamIndex));

					auto const* const streamOutputNode = static_cast<la::avdecc::controller::model::StreamOutputNode const*>(streamNode);
					redundantOutputStream->setStreamFormat(streamOutputNode->dynamicModel->streamInfo.streamFormat);

					redundantOutputStream->setGrandMasterID(avbInterfaceNode.dynamicModel->avbInfo.gptpGrandmasterID);
					redundantOutputStream->setGrandMasterDomain(avbInterfaceNode.dynamicModel->avbInfo.gptpDomainNumber);
					redundantOutputStream->setInterfaceLinkStatus(controlledEntity.getAvbInterfaceLinkStatus(avbInterfaceIndex));
					redundantOutputStream->setRunning(controlledEntity.isStreamOutputRunning(currentConfigurationIndex, streamIndex));
				}
			}

			// Single streams
			for (auto const&[streamIndex, streamNode] : configurationNode.streamOutputs)
			{
				if (!streamNode.isRedundant)
				{
					auto const avbInterfaceIndex{ streamNode.staticModel->avbInterfaceIndex };
					auto const& streamNode = controlledEntity.getStreamOutputNode(currentConfigurationIndex, streamIndex);
					auto const& avbInterfaceNode = controlledEntity.getAvbInterfaceNode(currentConfigurationIndex, avbInterfaceIndex);

					auto* outputStream = StreamNode::createOutputNode(*entity, streamIndex, avbInterfaceIndex);
					outputStream->setName(avdecc::helper::outputStreamName(controlledEntity, streamIndex));
					outputStream->setStreamFormat(streamNode.dynamicModel->streamInfo.streamFormat);
					outputStream->setGrandMasterID(avbInterfaceNode.dynamicModel->avbInfo.gptpGrandmasterID);
					outputStream->setGrandMasterDomain(avbInterfaceNode.dynamicModel->avbInfo.gptpDomainNumber);
					outputStream->setInterfaceLinkStatus(controlledEntity.getAvbInterfaceLinkStatus(avbInterfaceIndex));
					outputStream->setRunning(controlledEntity.isStreamOutputRunning(currentConfigurationIndex, streamIndex));
				}
			}

			return entity;
		}
		catch (...)
		{
			return nullptr;
		}
	}
	
	Node* buildListenerNode(la::avdecc::controller::ControlledEntity const& controlledEntity, la::avdecc::UniqueIdentifier const& entityID, la::avdecc::controller::model::ConfigurationNode const& configurationNode)
	{
		try
		{
			auto const& entityNode = controlledEntity.getEntityNode();
			auto const currentConfigurationIndex = entityNode.dynamicModel->currentConfiguration;

			auto* entity = EntityNode::create(entityID);
			entity->setName(avdecc::helper::smartEntityName(controlledEntity));

			// Redundant streams
			for (auto const&[redundantIndex, redundantNode] : configurationNode.redundantStreamInputs)
			{
				auto* redundantInput = RedundantNode::createInputNode(*entity, redundantIndex);
				redundantInput->setName(avdecc::helper::redundantInputName(redundantIndex));

				auto const& redundantStreamNode = controlledEntity.getRedundantStreamInputNode(currentConfigurationIndex, redundantIndex);

				for (auto const&[streamIndex, streamNode] : redundantNode.redundantStreams)
				{
					auto const avbInterfaceIndex{ streamNode->staticModel->avbInterfaceIndex };
					auto const& avbInterfaceNode = controlledEntity.getAvbInterfaceNode(currentConfigurationIndex, avbInterfaceIndex);

					auto* redundantInputStream = StreamNode::createRedundantInputNode(*redundantInput, streamIndex, avbInterfaceIndex);
					redundantInputStream->setName(avdecc::helper::inputStreamName(controlledEntity, streamIndex));

					auto const* const streamInputNode = static_cast<la::avdecc::controller::model::StreamInputNode const*>(streamNode);
					redundantInputStream->setStreamFormat(streamInputNode->dynamicModel->streamInfo.streamFormat);

					redundantInputStream->setGrandMasterID(avbInterfaceNode.dynamicModel->avbInfo.gptpGrandmasterID);
					redundantInputStream->setGrandMasterDomain(avbInterfaceNode.dynamicModel->avbInfo.gptpDomainNumber);
					redundantInputStream->setInterfaceLinkStatus(controlledEntity.getAvbInterfaceLinkStatus(avbInterfaceIndex));
					redundantInputStream->setRunning(controlledEntity.isStreamInputRunning(currentConfigurationIndex, streamIndex));
				}
			}

			// Single streams
			for (auto const&[streamIndex, streamNode] : configurationNode.streamInputs)
			{
				if (!streamNode.isRedundant)
				{
					auto const avbInterfaceIndex{ streamNode.staticModel->avbInterfaceIndex };
					auto const& streamNode = controlledEntity.getStreamInputNode(currentConfigurationIndex, streamIndex);
					auto const& avbInterfaceNode = controlledEntity.getAvbInterfaceNode(currentConfigurationIndex, avbInterfaceIndex);

					auto* inputStream = StreamNode::createInputNode(*entity, streamIndex, avbInterfaceIndex);
					inputStream->setName(avdecc::helper::inputStreamName(controlledEntity, streamIndex));
					inputStream->setStreamFormat(streamNode.dynamicModel->streamInfo.streamFormat);
					inputStream->setGrandMasterID(avbInterfaceNode.dynamicModel->avbInfo.gptpGrandmasterID);
					inputStream->setGrandMasterDomain(avbInterfaceNode.dynamicModel->avbInfo.gptpDomainNumber);
					inputStream->setInterfaceLinkStatus(controlledEntity.getAvbInterfaceLinkStatus(avbInterfaceIndex));
					inputStream->setRunning(controlledEntity.isStreamInputRunning(currentConfigurationIndex, streamIndex));
				}
			}

			return entity;
		}
		catch (...)
		{
			return nullptr;
		}
	}
	
	void addTalker(la::avdecc::controller::ControlledEntity const& controlledEntity, la::avdecc::UniqueIdentifier const& entityID, la::avdecc::controller::model::ConfigurationNode const& configurationNode)
	{
		auto* node = buildTalkerNode(controlledEntity, entityID, configurationNode);

		auto const childrenCount = priv::absoluteChildrenCount(node);
		
		auto const first = talkerSectionCount();
		auto const last = first + childrenCount;
		
		beginInsertTalkerItems(first, last);

		_talkerNodeMap.insert(std::make_pair(entityID, node));
		
		priv::insertNodes(_talkerNodes, node);
		
		rebuildTalkerSectionCache();

		// Update capabilities matrix
		_intersectionData.resize(last + 1);
		for (auto talkerSection = first; talkerSection <= last; ++talkerSection)
		{
			auto& row = _intersectionData.at(talkerSection);
			row.resize(_listenerNodes.size());
			
			auto* talker = _talkerNodes.at(talkerSection);
			for (auto listenerSection = 0u; listenerSection < _listenerNodes.size(); ++listenerSection)
			{
				auto* listener = _listenerNodes.at(listenerSection);
				priv::initializeIntersectionData(talker, listener, row.at(listenerSection));
			}
		}
		
#if ENABLE_CONNECTION_MATRIX_DEBUG
		dump();
#endif
		
		endInsertTalkerItems();
	}
	
	void addListener(la::avdecc::controller::ControlledEntity const& controlledEntity, la::avdecc::UniqueIdentifier const& entityID, la::avdecc::controller::model::ConfigurationNode const& configurationNode)
	{
		auto* node = buildListenerNode(controlledEntity, entityID, configurationNode);
		
		auto const childrenCount = priv::absoluteChildrenCount(node);
		
		auto const first = listenerSectionCount();
		auto const last = first + childrenCount;
		
		beginInsertListenerItems(first, last);

		_listenerNodeMap.insert(std::make_pair(entityID, node));
		
		priv::insertNodes(_listenerNodes, node);

		rebuildListenerSectionCache();
		
		// Update capabilities matrix
		for (auto talkerSection = 0u; talkerSection < _talkerNodes.size(); ++talkerSection)
		{
			auto& row = _intersectionData.at(talkerSection);
			row.resize(_listenerNodes.size());
			
			auto* talker = _talkerNodes.at(talkerSection);
			for (auto listenerSection = first; listenerSection <= last; ++listenerSection)
			{
				auto* listener = _listenerNodes.at(listenerSection);
				priv::initializeIntersectionData(talker, listener, row.at(listenerSection));
			}
		}

#if ENABLE_CONNECTION_MATRIX_DEBUG
		dump();
#endif
		
		endInsertListenerItems();
	}
	
	void removeTalker(la::avdecc::UniqueIdentifier const& entityID)
	{
		if (auto* node = talkerNodeFromEntityID(entityID))
		{
			auto const childrenCount = priv::absoluteChildrenCount(node);
			
			auto const first = priv::indexOf(_talkerNodeSectionMap, node);
			auto const last = first + childrenCount;

			beginRemoveTalkerItems(first, last);
			
			priv::removeNodes(_talkerNodes, first, last + 1 /* entity */);
			
			rebuildTalkerSectionCache();
			
			_talkerNodeMap.erase(entityID);
			
			_intersectionData.erase(
				std::next(std::begin(_intersectionData), first),
				std::next(std::begin(_intersectionData), last + 1));

#if ENABLE_CONNECTION_MATRIX_DEBUG
			dump();
#endif
			
			endRemoveTalkerItems();
		}
	}
	
	void removeListener(la::avdecc::UniqueIdentifier const& entityID)
	{
		if (auto* node = listenerNodeFromEntityID(entityID))
		{
			auto const childrenCount = priv::absoluteChildrenCount(node);
			
			auto const first = priv::indexOf(_listenerNodeSectionMap, node);
			auto const last = first + childrenCount;

			beginRemoveListenerItems(first, last);

			priv::removeNodes(_listenerNodes, first, last + 1 /* entity */);

			rebuildListenerSectionCache();

			_listenerNodeMap.erase(entityID);
			
			for (auto talkerSection = 0u; talkerSection < _talkerNodes.size(); ++talkerSection)
			{
				auto& row = _intersectionData.at(talkerSection);
				
				row.erase(
					std::next(std::begin(row), first),
					std::next(std::begin(row), last + 1));
			}

#if ENABLE_CONNECTION_MATRIX_DEBUG
			dump();
#endif

			endRemoveListenerItems();
		}
	}
	
	Node* talkerNode(int section) const
	{
		if (!isValidTalkerSection(section))
		{
			return nullptr;
		}
		
		return _talkerNodes.at(section);
	}
	
	Node* listenerNode(int section) const
	{
		if (!isValidListenerSection(section))
		{
			return nullptr;
		}
		
		return _listenerNodes.at(section);
	}
	
	// avdecc::ControllerManager slots

	void handleControllerOffline()
	{
		Q_Q(Model);

		emit q->beginResetModel();
		_talkerNodeMap.clear();
		_listenerNodeMap.clear();
		_talkerNodes.clear();
		_listenerNodes.clear();
		_talkerStreamSectionMap.clear();
		_listenerStreamSectionMap.clear();
		_talkerNodeSectionMap.clear();
		_listenerNodeSectionMap.clear();
		_intersectionData.clear();
		emit q->endResetModel();
	}

	void handleEntityOnline(la::avdecc::UniqueIdentifier const entityID)
	{
		try
		{
			auto& manager = avdecc::ControllerManager::getInstance();
			auto controlledEntity = manager.getControlledEntity(entityID);
			if (controlledEntity && AVDECC_ASSERT_WITH_RET(!controlledEntity->gotFatalEnumerationError(), "An entity should not be set online if it had an enumeration error"))
			{
				if (!controlledEntity->getEntity().getEntityCapabilities().test(la::avdecc::entity::EntityCapability::AemSupported))
				{
					return;
				}

				auto const& entityNode = controlledEntity->getEntityNode();
				auto const& configurationNode = controlledEntity->getConfigurationNode(entityNode.dynamicModel->currentConfiguration);
				
				// Talker
				if (controlledEntity->getEntity().getTalkerCapabilities().test(la::avdecc::entity::TalkerCapability::Implemented) && !configurationNode.streamOutputs.empty())
				{
					addTalker(*controlledEntity, entityID, configurationNode);
				}
				
				// Listener
				if (controlledEntity->getEntity().getListenerCapabilities().test(la::avdecc::entity::ListenerCapability::Implemented) && !configurationNode.streamInputs.empty())
				{
					addListener(*controlledEntity, entityID, configurationNode);
				}
			}
		}
		catch (la::avdecc::controller::ControlledEntity::Exception const&)
		{
			// Ignore exception
		}
		catch (...)
		{
			// Uncaught exception
			AVDECC_ASSERT(false, "Uncaught exception");
		}
	}

	void handleEntityOffline(la::avdecc::UniqueIdentifier const entityID)
	{
		if (hasTalker(entityID))
		{
			removeTalker(entityID);
		}

		if (hasListener(entityID))
		{
			removeListener(entityID);
		}
	}

	void handleStreamRunningChanged(la::avdecc::UniqueIdentifier const entityID, la::avdecc::entity::model::DescriptorType const descriptorType, la::avdecc::entity::model::StreamIndex const streamIndex, bool const isRunning)
	{
		if (descriptorType == la::avdecc::entity::model::DescriptorType::StreamOutput)
		{
			auto* node = talkerStreamNode(entityID, streamIndex);
			node->setRunning(isRunning);
			emit talkerHeaderDataChanged(node);
		}
		else if (descriptorType == la::avdecc::entity::model::DescriptorType::StreamInput)
		{
			auto* node = listenerStreamNode(entityID, streamIndex);
			node->setRunning(isRunning);
			emit listenerHeaderDataChanged(node);
		}
	}

	void handleStreamConnectionChanged(la::avdecc::controller::model::StreamConnectionState const& state)
	{
		auto const dirtyFlags = priv::IntersectionDirtyFlags{ priv::IntersectionDirtyFlag::UpdateConnected };

		auto const entityID = state.listenerStream.entityID;
		auto const streamIndex = state.listenerStream.streamIndex;

		auto* listener = listenerStreamNode(entityID, streamIndex);

		listenerIntersectionDataChanged(listener, true, true, dirtyFlags);
	}

	void handleStreamFormatChanged(la::avdecc::UniqueIdentifier const entityID, la::avdecc::entity::model::DescriptorType const descriptorType, la::avdecc::entity::model::StreamIndex const streamIndex, la::avdecc::entity::model::StreamFormat const streamFormat)
	{
		auto const dirtyFlags = priv::IntersectionDirtyFlags{ priv::IntersectionDirtyFlag::UpdateLinkStatus };

		if (hasTalker(entityID))
		{
			auto* node = talkerStreamNode(entityID, streamIndex);
			node->setStreamFormat(streamFormat);
			talkerIntersectionDataChanged(node, true, false, dirtyFlags);
		}

		if (hasListener(entityID))
		{
			auto* node = listenerStreamNode(entityID, streamIndex);
			node->setStreamFormat(streamFormat);
			listenerIntersectionDataChanged(node, true, false, dirtyFlags);
		}
	}

	void handleGptpChanged(la::avdecc::UniqueIdentifier const entityID, la::avdecc::entity::model::AvbInterfaceIndex const avbInterfaceIndex, la::avdecc::UniqueIdentifier const grandMasterID, std::uint8_t const grandMasterDomain)
	{
		auto const dirtyFlags = priv::IntersectionDirtyFlags{ priv::IntersectionDirtyFlag::UpdateGptp };

		if (hasTalker(entityID))
		{
			auto* talker = talkerNodeFromEntityID(entityID);

			talker->accept(avbInterfaceIndex, [this, grandMasterID, grandMasterDomain, dirtyFlags](StreamNode* node)
			{
				node->setGrandMasterID(grandMasterID);
				node->setGrandMasterDomain(grandMasterDomain);
				talkerIntersectionDataChanged(node, true, false, dirtyFlags);
			});
		}

		if (hasListener(entityID))
		{
			auto* listener = listenerNodeFromEntityID(entityID);

			listener->accept(avbInterfaceIndex, [this, grandMasterID, grandMasterDomain, dirtyFlags](StreamNode* node)
			{
				node->setGrandMasterID(grandMasterID);
				node->setGrandMasterDomain(grandMasterDomain);
				listenerIntersectionDataChanged(node, true, false, dirtyFlags);
			});
		}
	}

	void handleEntityNameChanged(la::avdecc::UniqueIdentifier const entityID)
	{
		try
		{
			auto& manager = avdecc::ControllerManager::getInstance();
			auto controlledEntity = manager.getControlledEntity(entityID);
			if (controlledEntity)
			{
				auto const name = avdecc::helper::smartEntityName(*controlledEntity);

				if (hasTalker(entityID))
				{
					auto* node = talkerNodeFromEntityID(entityID);
					node->setName(name);

					talkerHeaderDataChanged(node);
				}

				if (hasListener(entityID))
				{
					auto* node = listenerNodeFromEntityID(entityID);
					node->setName(name);

					listenerHeaderDataChanged(node);
				}
			}
		}
		catch (...)
		{
			// Uncaught exception
			AVDECC_ASSERT(false, "Uncaught exception");
		}
	}

	void handleStreamNameChanged(la::avdecc::UniqueIdentifier const entityID, la::avdecc::entity::model::ConfigurationIndex const configurationIndex, la::avdecc::entity::model::DescriptorType const descriptorType, la::avdecc::entity::model::StreamIndex const streamIndex)
	{
		try
		{
			auto& manager = avdecc::ControllerManager::getInstance();
			auto controlledEntity = manager.getControlledEntity(entityID);
			if (controlledEntity)
			{
				if (descriptorType == la::avdecc::entity::model::DescriptorType::StreamOutput)
				{
					auto const name = avdecc::helper::outputStreamName(*controlledEntity, streamIndex);

					auto* node = talkerStreamNode(entityID, streamIndex);
					node->setName(name);

					talkerHeaderDataChanged(node);
				}
				else if (descriptorType == la::avdecc::entity::model::DescriptorType::StreamInput)
				{
					auto const name = avdecc::helper::inputStreamName(*controlledEntity, streamIndex);

					auto* node = listenerStreamNode(entityID, streamIndex);
					node->setName(name);

					listenerHeaderDataChanged(node);
				}
			}
		}
		catch (...)
		{
			// Uncaught exception
			AVDECC_ASSERT(false, "Uncaught exception");
		}
	}

	void handleAvbInterfaceLinkStatusChanged(la::avdecc::UniqueIdentifier const entityID, la::avdecc::entity::model::AvbInterfaceIndex const avbInterfaceIndex, la::avdecc::controller::ControlledEntity::InterfaceLinkStatus const linkStatus)
	{
		auto const dirtyFlags = priv::IntersectionDirtyFlags{ priv::IntersectionDirtyFlag::UpdateLinkStatus };

		if (hasTalker(entityID))
		{
			auto* talker = talkerNodeFromEntityID(entityID);

			talker->accept(avbInterfaceIndex, [this, linkStatus, dirtyFlags](StreamNode* node)
			{
				node->setInterfaceLinkStatus(linkStatus);
				talkerIntersectionDataChanged(node, true, false, dirtyFlags);
			});
		}

		if (hasListener(entityID))
		{
			auto* listener = listenerNodeFromEntityID(entityID);

			listener->accept(avbInterfaceIndex, [this, linkStatus, dirtyFlags](StreamNode* node)
			{
				node->setInterfaceLinkStatus(linkStatus);
				listenerIntersectionDataChanged(node, true, false, dirtyFlags);
			});
		}
	}
	
private:
	bool hasTalker(la::avdecc::UniqueIdentifier const entityID) const
	{
		return _talkerNodeMap.count(entityID) == 1;
	}

	bool hasListener(la::avdecc::UniqueIdentifier const entityID) const
	{
		return _listenerNodeMap.count(entityID) == 1;
	}
	
	int talkerEntitySection(la::avdecc::UniqueIdentifier const& entityID) const
	{
		auto* node = talkerNodeFromEntityID(entityID);
		return priv::indexOf(_talkerNodeSectionMap, node);
	}
	
	int listenerEntitySection(la::avdecc::UniqueIdentifier const& entityID) const
	{
		auto* node = listenerNodeFromEntityID(entityID);
		return priv::indexOf(_listenerNodeSectionMap, node);
	}

	int talkerNodeSection(Node* const node) const
	{
		return priv::indexOf(_talkerNodeSectionMap, node);
	}

	int listenerNodeSection(Node* const node) const
	{
		return priv::indexOf(_listenerNodeSectionMap, node);
	}

	int talkerStreamSection(la::avdecc::UniqueIdentifier const& entityID, la::avdecc::entity::model::StreamIndex const streamIndex) const
	{
		auto const key = std::make_pair(entityID, streamIndex);
		auto const it = _talkerStreamSectionMap.find(key);
		assert(it != std::end(_talkerStreamSectionMap));
		return it->second;
	}
	
	int listenerStreamSection(la::avdecc::UniqueIdentifier const& entityID, la::avdecc::entity::model::StreamIndex const streamIndex) const
	{
		auto const key = std::make_pair(entityID, streamIndex);
		auto const it = _listenerStreamSectionMap.find(key);
		assert(it != std::end(_listenerStreamSectionMap));
		return it->second;
	}

	EntityNode* talkerNodeFromEntityID(la::avdecc::UniqueIdentifier const& entityID) const
	{
		auto const it = _talkerNodeMap.find(entityID);
		assert(it != std::end(_talkerNodeMap));
		auto* node = it->second.get();
		assert(node->type() == Node::Type::Entity);
		return static_cast<EntityNode*>(node);
	}

	EntityNode* listenerNodeFromEntityID(la::avdecc::UniqueIdentifier const& entityID) const
	{
		auto const it = _listenerNodeMap.find(entityID);
		assert(it != std::end(_listenerNodeMap));
		auto* node = it->second.get();
		assert(node->type() == Node::Type::Entity);
		return static_cast<EntityNode*>(node);
	}

	StreamNode* talkerStreamNode(la::avdecc::UniqueIdentifier const& entityID, la::avdecc::entity::model::StreamIndex const streamIndex) const
	{
		auto const section = talkerStreamSection(entityID, streamIndex);
		auto* node = _talkerNodes.at(section);
		assert(node->isStreamNode());
		return static_cast<StreamNode*>(node);
	}

	StreamNode* listenerStreamNode(la::avdecc::UniqueIdentifier const& entityID, la::avdecc::entity::model::StreamIndex const streamIndex) const
	{
		auto const section = listenerStreamSection(entityID, streamIndex);
		auto* node = _listenerNodes.at(section);
		assert(node->isStreamNode());
		return static_cast<StreamNode*>(node);
	}

private:
	QModelIndex createIndex(int const talkerSection, int const listenerSection) const
	{
		Q_Q(const Model);

		if (!_transposed)
		{
			return q->createIndex(talkerSection, listenerSection);
		}
		else
		{
			return q->createIndex(listenerSection, talkerSection);
		}
	}

	void intersectionDataChanged(int const talkerSection, int const listenerSection, priv::IntersectionDirtyFlags dirtyFlags)
	{
		Q_Q(Model);

		auto& data = _intersectionData.at(talkerSection).at(listenerSection);

		priv::computeIntersectionCapabilities(data, dirtyFlags);

		auto const index = createIndex(talkerSection, listenerSection);
		emit q->dataChanged(index, index);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		highlightIntersection(talkerSection, listenerSection);
#endif
	}

	void talkerIntersectionDataChanged(Node* talker, bool const andParents, bool const andChildren, priv::IntersectionDirtyFlags dirtyFlags)
	{
		assert(talker);

		// Recursively update the parents
		if (andParents)
		{
			auto* node = talker;
			while (auto* parent = node->parent())
			{
				talkerIntersectionDataChanged(parent, andParents, false, dirtyFlags);
				node = parent;
			}
		}

		// Update the children
		if (andChildren)
		{
			talker->accept([=](Node* child)
			{
				if (child != talker)
				{
					talkerIntersectionDataChanged(child, false, andChildren, dirtyFlags);
				}
			});
		}

		auto const talkerSection = talkerNodeSection(talker);
		for (auto listenerSection = 0u; listenerSection < _listenerNodes.size(); ++listenerSection)
		{
			// TODO, optimizable
			intersectionDataChanged(talkerSection, listenerSection, dirtyFlags);
		}
	}

	void listenerIntersectionDataChanged(Node* listener, bool const andParents, bool const andChildren, priv::IntersectionDirtyFlags dirtyFlags)
	{
		assert(listener);

		// Recursively update the parents
		if (andParents)
		{
			auto* node = listener;
			while (auto* parent = node->parent())
			{
				listenerIntersectionDataChanged(parent, andParents, false, dirtyFlags);
				node = parent;
			}
		}

		// Update the children
		if (andChildren)
		{
			listener->accept([=](Node* child)
			{
				if (child != listener)
				{
					listenerIntersectionDataChanged(child, false, andChildren, dirtyFlags);
				}
			});
		}

		auto const listenerSection = listenerNodeSection(listener);
		for (auto talkerSection = 0u; talkerSection < _talkerNodes.size(); ++talkerSection)
		{
			// TODO, compute topLeft / bottomRight indexes for efficiency
			intersectionDataChanged(talkerSection, listenerSection, dirtyFlags);
		}
	}

	void talkerHeaderDataChanged(Node* const node)
	{
		Q_Q(Model);

		auto section = talkerNodeSection(node);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "talkerHeaderDataChanged(" << section << ")";
#endif

		emit q->headerDataChanged(talkerOrientation(), section, section);
	}

	void listenerHeaderDataChanged(Node* const node)
	{
		Q_Q(Model);

		auto section = listenerNodeSection(node);

#if ENABLE_CONNECTION_MATRIX_DEBUG
		qDebug() << "listenerHeaderDataChanged(" << section << ")";
#endif

		emit q->headerDataChanged(listenerOrientation(), section, section);
	}

	Qt::Orientation talkerOrientation() const
	{
		return !_transposed ? Qt::Vertical : Qt::Horizontal;
	}

	Qt::Orientation listenerOrientation() const
	{
		return !_transposed ? Qt::Horizontal : Qt::Vertical;
	}
	
	int talkerSectionCount() const
	{
		return static_cast<int>(_talkerNodes.size());
	}
	
	int listenerSectionCount() const
	{
		return static_cast<int>(_listenerNodes.size());
	}
	
	int talkerIndex(QModelIndex const& index) const
	{
		return !_transposed ? index.row() : index.column();
	}
	
	int listenerIndex(QModelIndex const& index) const
	{
		return !_transposed ? index.column() : index.row();
	}
	
	bool isValidTalkerSection(int section) const
	{
		return section >= 0 && section < talkerSectionCount();
	}
	
	bool isValidListenerSection(int section) const
	{
		return section >= 0 && section < listenerSectionCount();
	}
	
	QString talkerHeaderData(int section) const
	{
		if (!isValidTalkerSection(section))
		{
			return {};
		}

		return _talkerNodes.at(section)->name();
	}
	
	QString listenerHeaderData(int section) const
	{
		if (!isValidListenerSection(section))
		{
			return {};
		}

		return _listenerNodes.at(section)->name();
	}
	
private:
	Model* const q_ptr{ nullptr };
	Q_DECLARE_PUBLIC(Model);
	
	bool _transposed{ false };
	
	// Entity nodes by entity ID
	priv::NodeMap _talkerNodeMap;
	priv::NodeMap _listenerNodeMap;
	
	// Flattened nodes
	priv::Nodes _talkerNodes;
	priv::Nodes _listenerNodes;
	
	// Stream section quick access map
	priv::StreamSectionMap _talkerStreamSectionMap;
	priv::StreamSectionMap _listenerStreamSectionMap;

	// Node section quick access map
	priv::NodeSectionMap _talkerNodeSectionMap;
	priv::NodeSectionMap _listenerNodeSectionMap;

	// Talker major intersection data matrix
	std::vector<std::vector<Model::IntersectionData>> _intersectionData;
};

Model::Model(QObject* parent)
: QAbstractTableModel{ parent }
, d_ptr{ new ModelPrivate{ this } }
{
}

Model::~Model() = default;

int Model::rowCount(QModelIndex const&) const
{
	Q_D(const Model);
	
	if (!d->_transposed)
	{
		return d->talkerSectionCount();
	}
	else
	{
		return d->listenerSectionCount();
	}
}

int Model::columnCount(QModelIndex const&) const
{
	Q_D(const Model);
	
	if (!d->_transposed)
	{
		return d->listenerSectionCount();
	}
	else
	{
		return d->talkerSectionCount();
	}
}

QVariant Model::data(QModelIndex const& index, int role) const
{
#if ENABLE_CONNECTION_MATRIX_DEBUG
	if (role == Qt::BackgroundRole)
	{
		auto const& data = intersectionData(index);
		if (data.animation)
		{
			return data.animation->currentValue();
		}
	}
#endif

	return {};
}

QVariant Model::headerData(int section, Qt::Orientation orientation, int role) const
{
	Q_D(const Model);
	
	if (role == Qt::DisplayRole)
	{
		if (!d->_transposed)
		{
			if (orientation == Qt::Vertical)
			{
				return d->talkerHeaderData(section);
			}
			else
			{
				return d->listenerHeaderData(section);
			}
		}
		else
		{
			if (orientation == Qt::Vertical)
			{
				return d->listenerHeaderData(section);
			}
			else
			{
				return d->talkerHeaderData(section);
			}
		}
	}
	
	return {};
}

Node* Model::node(int section, Qt::Orientation orientation) const
{
	Q_D(const Model);
	
	if (!d->_transposed)
	{
		if (orientation == Qt::Vertical)
		{
			return d->talkerNode(section);
		}
		else
		{
			return d->listenerNode(section);
		}
	}
	else
	{
		if (orientation == Qt::Vertical)
		{
			return d->listenerNode(section);
		}
		else
		{
			return d->talkerNode(section);
		}
	}
}


int Model::section(Node* node, Qt::Orientation orientation) const
{
	Q_D(const Model);
	
	if (!d->_transposed)
	{
		if (orientation == Qt::Vertical)
		{
			return priv::indexOf(d->_talkerNodeSectionMap, node);
		}
		else
		{
			return priv::indexOf(d->_listenerNodeSectionMap, node);
		}
	}
	else
	{
		if (orientation == Qt::Vertical)
		{
			return priv::indexOf(d->_listenerNodeSectionMap, node);
		}
		else
		{
			return priv::indexOf(d->_talkerNodeSectionMap, node);
		}
	}
}

Model::IntersectionData const& Model::intersectionData(QModelIndex const& index) const
{
	Q_D(const Model);

	auto const talkerSection = d->talkerIndex(index);
	auto const listenerSection = d->listenerIndex(index);
	
	assert(d->isValidTalkerSection(talkerSection));
	assert(d->isValidListenerSection(listenerSection));
	
	return d->_intersectionData.at(talkerSection).at(listenerSection);
}

void Model::setTransposed(bool const transposed)
{
	Q_D(Model);
	
	if (transposed != d->_transposed)
	{
		emit beginResetModel();
		d->_transposed = transposed;
		emit endResetModel();
	}
}

bool Model::isTransposed() const
{
	Q_D(const Model);
	return d->_transposed;
}

} // namespace connectionMatrix

#include "model.moc"
