package com.kevinbatdorf.plugins.retroemulator

import org.json.JSONArray
import org.json.JSONObject

/**
 * Parameter coercion for bridge functions.
 *
 * The NativePHP BridgeRouter builds the parameters map with
 * `JSONObject.get(key)`, so nested values arrive as org.json
 * JSONArray/JSONObject — a plain `as? List`/`as? Map` cast silently nulls
 * them out (the bug the first on-device conformance run caught). Kept free
 * of host-app types so the plugin's own test suite can pin the contract.
 */
object BridgeParams {

    fun fromJson(value: Any?): Any? = when (value) {
        JSONObject.NULL -> null
        is JSONObject -> value.keys().asSequence().associateWith { fromJson(value.get(it)) }
        is JSONArray -> (0 until value.length()).map { fromJson(value.get(it)) }
        else -> value
    }

    fun list(parameters: Map<String, Any>, key: String): List<Any?>? =
        (fromJson(parameters[key]) as? List<*>)

    fun map(parameters: Map<String, Any>, key: String): Map<String, Any>? {
        val map = fromJson(parameters[key]) as? Map<*, *> ?: return null

        return map.entries
            .filter { it.key is String && it.value != null }
            .associate { it.key as String to it.value as Any }
    }

    /**
     * Unmarshal a raw payload JSON string exactly the way
     * BridgeRouter.nativePHPCall does — `JSONObject.get(key)` per key, no
     * conversion. Tests feed real payloads through this to guarantee the
     * fixtures match what bridge functions actually receive.
     */
    fun unmarshalLikeBridgeRouter(parametersJson: String): Map<String, Any> {
        val parameters = mutableMapOf<String, Any>()
        val jsonObject = JSONObject(parametersJson)
        val keys = jsonObject.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            parameters[key] = jsonObject.get(key)
        }
        return parameters
    }
}
