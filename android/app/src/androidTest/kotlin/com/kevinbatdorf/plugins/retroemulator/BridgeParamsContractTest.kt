package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Contract fixtures: every payload below is byte-for-byte what the demo
 * app's conformance runner sends over the real bridge, unmarshalled exactly
 * the way BridgeRouter.nativePHPCall does (JSONObject.get per key). This is
 * the layer where a plain `as? List`/`as? Map` cast silently nulls out
 * org.json values — these tests pin that reality so mocks can't drift from it.
 */
@RunWith(AndroidJUnit4::class)
class BridgeParamsContractTest {

    private fun params(payload: String) = BridgeParams.unmarshalLikeBridgeRouter(payload)

    @Test
    fun writeMemoryBytesArriveAsIntList() {
        val p = params("""{"surface":"main","address":8265472,"bytes":[171]}""")
        val bytes = BridgeParams.list(p, "bytes")
        assert(bytes != null) { "bytes must coerce from JSONArray" }
        assert(bytes!!.single() == 171) { "got $bytes" }
    }

    @Test
    fun watchAddressesCoerceIncludingDictEntries() {
        val p = params("""{"surface":"main","addresses":[8265488,{"address":8257344,"length":2}]}""")
        val addresses = BridgeParams.list(p, "addresses")
        assert(addresses != null)
        assert(addresses!![0] == 8265488) { "plain address entry, got ${addresses[0]}" }
        val dict = addresses[1] as? Map<*, *>
        assert(dict != null) { "dict entry must coerce to Map, got ${addresses[1]}" }
        assert(dict!!["address"] == 8257344 && dict["length"] == 2)
    }

    @Test
    fun setButtonsStateCoercesToBooleanMap() {
        val p = params("""{"surface":"main","port":1,"state":{"Up":true,"A":false}}""")
        val state = BridgeParams.map(p, "state")
        assert(state != null) { "state must coerce from JSONObject" }
        assert(state!!["Up"] == true && state["A"] == false) { "got $state" }
    }

    @Test
    fun loadSystemConfigCoercesWithBooleans() {
        val p = params("""{"surface":"main","system":"sfc","config":{"autoSave":false}}""")
        val config = BridgeParams.map(p, "config")
        assert(config != null && config["autoSave"] == false) { "got $config" }
    }

    @Test
    fun configureOptionsCarryNumbers() {
        val p = params("""{"surface":"main","options":{"speed":2.0,"runAhead":2}}""")
        val options = BridgeParams.map(p, "options")
        assert(options != null)
        assert((options!!["runAhead"] as Number).toInt() == 2)
        assert((options["speed"] as Number).toDouble() == 2.0) { "got $options" }
    }

    @Test
    fun jsonNullBecomesKotlinNull() {
        val p = params("""{"surface":"main","path":null}""")
        assert(BridgeParams.fromJson(p["path"]) == null) { "JSONObject.NULL must coerce to null" }
    }

    @Test
    fun missingKeysReturnNullNotEmpty() {
        val p = params("""{"surface":"main"}""")
        assert(BridgeParams.list(p, "bytes") == null)
        assert(BridgeParams.map(p, "options") == null)
    }
}
