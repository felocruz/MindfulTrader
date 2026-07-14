#pragma once

#include "MessageType.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace MTS {
namespace Messaging {

/**
 * @class MessageRouter
 * @brief Content-based message routing (Strategy Pattern)
 * 
 * Responsibilities:
 * - Register handlers for specific message types
 * - Extract MessageType from serialized data
 * - Dispatch to appropriate handler
 * - Support middleware/interceptors
 * 
 * Design Pattern: Strategy (handler dispatch)
 * Thread Safety: Handler registration must happen during setup, not during dispatch
 * 
 * Typical Usage:
 *   router.RegisterHandler(MessageType::HEARTBEAT, handle_heartbeat);
 *   router.RegisterHandler(MessageType::OBSERVATION, handle_observation);
 *   
 *   // In receive loop:
 *   router.Route(received_data, received_size);  // Auto-calls appropriate handler
 */
class MessageRouter {
public:
    /**
     * @brief Handler function type
     * 
     * Parameters:
     *   - const uint8_t* data: Serialized message data
     *   - size_t size: Message size in bytes
     *   - const MessageMetadata& metadata: Extracted metadata (timestamp, sender, etc.)
     * 
     * Return: true if handled successfully, false if handler error
     */
    using MessageHandler = std::function<bool(
        const uint8_t* data,
        size_t size,
        const MessageMetadata& metadata
    )>;

    /**
     * @brief Middleware function type
     * 
     * Called before handler for pre-processing.
     * Can filter messages (return false to skip handler).
     * 
     * Parameters:
     *   - MessageType: Message type being routed
     *   - const MessageMetadata& metadata: Message metadata
     * 
     * Return: true to proceed with handler, false to skip
     */
    using MessageMiddleware = std::function<bool(
        MessageType type,
        const MessageMetadata& metadata
    )>;

    MessageRouter() = default;
    ~MessageRouter() = default;

    /**
     * @brief Register handler for message type
     * @param type Message type
     * @param handler Handler function
     * @return true if registered (false if duplicate)
     */
    bool RegisterHandler(MessageType type, MessageHandler handler);

    /**
     * @brief Unregister handler for message type
     * @param type Message type
     * @return true if found and removed
     */
    bool UnregisterHandler(MessageType type);

    /**
     * @brief Register middleware (called before all handlers)
     * @param name Middleware name (for debugging)
     * @param middleware Middleware function
     */
    void RegisterMiddleware(const std::string& name, MessageMiddleware middleware);

    /**
     * @brief Clear all middleware
     */
    void ClearMiddleware();

    /**
     * @brief Route incoming message to appropriate handler
     * 
     * Process:
     * 1. Extract MessageType from serialized data (zero-copy peek)
     * 2. Call all registered middleware (pre-filters)
     * 3. If middleware approved, call handler for message type
     * 4. Return handler result
     * 
     * @param data Serialized message data
     * @param size Message size in bytes
     * @return true if handled successfully (or no handler registered), false if error
     */
    bool Route(const uint8_t* data, size_t size);

    /**
     * @brief Get diagnostics (registered handlers, middleware count)
     * @return String describing router state
     */
    std::string GetDiagnostics() const;

    /**
     * @brief Get handler count
     * @return Number of registered message handlers
     */
    size_t GetHandlerCount() const;

    /**
     * @brief Has handler for message type?
     * @param type Message type
     * @return true if handler registered
     */
    bool HasHandler(MessageType type) const;

private:
    // Handler registry
    std::map<MessageType, MessageHandler> handlers_;
    
    // Middleware pipeline
    std::vector<std::pair<std::string, MessageMiddleware>> middleware_;

    /**
     * @brief Extract MessageType from serialized FlatBuffer
     * 
     * Performs zero-copy lookup of message type field.
     * Safe to call on incomplete messages (validates bounds).
     * 
     * @param data Serialized data
     * @param size Data size
     * @return MessageType or HEARTBEAT if extraction failed
     */
    static MessageType ExtractMessageType(const uint8_t* data, size_t size);

    /**
     * @brief Extract full metadata from serialized FlatBuffer
     * @param data Serialized data
     * @param size Data size
     * @return MessageMetadata with extracted values
     */
    static MessageMetadata ExtractFullMetadata(const uint8_t* data, size_t size);
};

} // namespace Messaging
} // namespace MTS
