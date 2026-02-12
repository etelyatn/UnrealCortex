"""MCP tools for material graph operations."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)


def register_material_graph_tools(mcp, connection: UEConnection):
    """Register all material graph-related MCP tools."""

    @mcp.tool()
    def list_material_nodes(asset_path: str) -> str:
        """List all expression nodes in a material's graph.

        Lists all material expression nodes (texture samplers, math operations,
        parameters, etc.) in the material editor graph.

        Args:
            asset_path: Full asset path to the material

        Returns:
            JSON with:
            - nodes: Array of node objects with node_id, expression_class, and position
            - count: Total number of nodes
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.list_nodes", params)
            return format_response(result.get("data", {}), "list_material_nodes")
        except Exception as e:
            logger.error(f"list_material_nodes failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def get_material_node(
        asset_path: str,
        node_id: str,
    ) -> str:
        """Get detailed information about a specific material expression node.

        Args:
            asset_path: Full asset path to the material
            node_id: ID of the node to retrieve (from list_material_nodes)

        Returns:
            JSON with node details:
            - node_id: Node identifier
            - expression_class: Type of expression (e.g., "MaterialExpressionScalarParameter")
            - position: Object with x and y coordinates
            - properties: Object with node-specific properties (varies by expression type)
        """
        try:
            params = {
                "asset_path": asset_path,
                "node_id": node_id,
            }
            result = connection.send_command("material.get_node", params)
            return format_response(result.get("data", {}), "get_material_node")
        except Exception as e:
            logger.error(f"get_material_node failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def add_material_node(
        asset_path: str,
        expression_class: str,
        position_x: int = 0,
        position_y: int = 0,
    ) -> str:
        """Add a new expression node to a material's graph.

        Creates a new material expression node in the material editor graph.

        Args:
            asset_path: Full asset path to the material
            expression_class: Class name of expression to create (e.g., "MaterialExpressionScalarParameter")
            position_x: X coordinate for node position (default: 0)
            position_y: Y coordinate for node position (default: 0)

        Returns:
            JSON with:
            - node_id: ID of the created node
            - expression_class: Class name of created expression
            - position: Object with x and y coordinates
        """
        try:
            params = {
                "asset_path": asset_path,
                "expression_class": expression_class,
            }
            if position_x != 0 or position_y != 0:
                params["position"] = {"x": position_x, "y": position_y}
            result = connection.send_command("material.add_node", params)
            return format_response(result.get("data", {}), "add_material_node")
        except Exception as e:
            logger.error(f"add_material_node failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def remove_material_node(
        asset_path: str,
        node_id: str,
    ) -> str:
        """Remove an expression node from a material's graph.

        Deletes a material expression node and disconnects all its connections.

        Args:
            asset_path: Full asset path to the material
            node_id: ID of the node to remove

        Returns:
            JSON with:
            - node_id: ID of the removed node
            - removed: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "node_id": node_id,
            }
            result = connection.send_command("material.remove_node", params)
            return format_response(result.get("data", {}), "remove_material_node")
        except Exception as e:
            logger.error(f"remove_material_node failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def list_material_connections(asset_path: str) -> str:
        """List all connections between nodes in a material's graph.

        Lists all pin connections in the material editor graph, including
        connections to the material result node.

        Args:
            asset_path: Full asset path to the material

        Returns:
            JSON with:
            - connections: Array of connection objects with source_node, source_output, target_node, target_input
            - count: Total number of connections
        """
        try:
            params = {"asset_path": asset_path}
            result = connection.send_command("material.list_connections", params)
            return format_response(result.get("data", {}), "list_material_connections")
        except Exception as e:
            logger.error(f"list_material_connections failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def connect_material_nodes(
        asset_path: str,
        source_node: str,
        source_output: int,
        target_node: str,
        target_input: Any,
    ) -> str:
        """Connect an output pin to an input pin in a material graph.

        Creates a connection between two nodes or between a node and the material result.

        Args:
            asset_path: Full asset path to the material
            source_node: ID of the source node
            source_output: Output pin index on source node (usually 0)
            target_node: ID of the target node (or "MaterialResult" for material outputs)
            target_input: Input pin identifier - index (int) or name (str) for MaterialResult

        Returns:
            JSON with:
            - source_node: Source node ID
            - target_node: Target node ID
            - connected: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "source_node": source_node,
                "source_output": source_output,
                "target_node": target_node,
                "target_input": target_input,
            }
            result = connection.send_command("material.connect", params)
            return format_response(result.get("data", {}), "connect_material_nodes")
        except Exception as e:
            logger.error(f"connect_material_nodes failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def disconnect_material_nodes(
        asset_path: str,
        target_node: str,
        target_input: Any,
    ) -> str:
        """Disconnect an input pin in a material graph.

        Removes a connection from an input pin.

        Args:
            asset_path: Full asset path to the material
            target_node: ID of the target node (or "MaterialResult")
            target_input: Input pin identifier - index (int) or name (str) for MaterialResult

        Returns:
            JSON with:
            - target_node: Target node ID
            - disconnected: True if successful
        """
        try:
            params = {
                "asset_path": asset_path,
                "target_node": target_node,
                "target_input": target_input,
            }
            result = connection.send_command("material.disconnect", params)
            return format_response(result.get("data", {}), "disconnect_material_nodes")
        except Exception as e:
            logger.error(f"disconnect_material_nodes failed: {e}", exc_info=True)
            return json.dumps({"error": str(e)})
