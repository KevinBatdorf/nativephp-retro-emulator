package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Position→direction resolution for the on-screen d-pad.
 *
 * The properties that decide whether the pad feels right: diagonals are
 * reachable at all, a direction survives the finger sliding off the pad, and
 * the centre is quiet. Coordinates are normalized offsets from the pad centre
 * with +y downward.
 */
@RunWith(AndroidJUnit4::class)
class DpadResolverTest {

    private fun resolve(x: Float, y: Float) = DpadResolver.resolve(x, y)

    @Test
    fun centreIsNeutral() {
        assertEquals(emptySet<DpadDirection>(), resolve(0f, 0f))
    }

    @Test
    fun insideTheDeadZoneIsNeutral() {
        assertEquals(emptySet<DpadDirection>(), resolve(0.05f, 0.05f))
    }

    @Test
    fun justOutsideTheDeadZoneRegisters() {
        assertEquals(setOf(DpadDirection.Right), resolve(0.2f, 0f))
    }

    @Test
    fun cardinalsResolveToOneDirection() {
        assertEquals(setOf(DpadDirection.Right), resolve(1f, 0f))
        assertEquals(setOf(DpadDirection.Left), resolve(-1f, 0f))
        // +y is downward, so a negative y is Up.
        assertEquals(setOf(DpadDirection.Up), resolve(0f, -1f))
        assertEquals(setOf(DpadDirection.Down), resolve(0f, 1f))
    }

    @Test
    fun cornersResolveToTwoDirections() {
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Right), resolve(1f, -1f))
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Left), resolve(-1f, -1f))
        assertEquals(setOf(DpadDirection.Down, DpadDirection.Right), resolve(1f, 1f))
        assertEquals(setOf(DpadDirection.Down, DpadDirection.Left), resolve(-1f, 1f))
    }

    /** Four independent buttons can't express this; it's the whole point. */
    @Test
    fun aDiagonalIsReachableFromAnImperfectCorner() {
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Right), resolve(0.8f, -0.6f))
    }

    /** Sliding off the pad mid-jump must not drop the direction. */
    @Test
    fun aPositionBeyondThePadKeepsItsDirection() {
        assertEquals(setOf(DpadDirection.Right), resolve(4f, 0f))
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Right), resolve(3f, -3f))
    }

    @Test
    fun aShakyThumbAimingUpStaysCardinal() {
        assertEquals(setOf(DpadDirection.Up), resolve(0.15f, -1f))
    }

    @Test
    fun diagonalStrengthOneGivesEveryAnchorAnEqualSlice() {
        // ~27° above the +x axis sits inside the diagonal's slice only while
        // diagonals aren't penalised.
        val equal = DpadResolver.resolve(0.9f, -0.45f, diagonalStrength = 1f)
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Right), equal)

        val penalised = DpadResolver.resolve(0.9f, -0.45f, diagonalStrength = 2f)
        assertEquals(setOf(DpadDirection.Right), penalised)
    }

    @Test
    fun aLargerDeadZoneSwallowsASmallOffset() {
        assertEquals(emptySet<DpadDirection>(), DpadResolver.resolve(0.2f, 0f, deadZone = 0.5f))
    }
}
