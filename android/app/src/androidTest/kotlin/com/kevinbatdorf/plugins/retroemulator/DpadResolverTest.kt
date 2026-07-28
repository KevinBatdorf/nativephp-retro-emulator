package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Position→direction resolution for the on-screen d-pad.
 *
 * The properties that decide whether the pad feels right: a second direction
 * engages without waiting for the finger to rotate, a direction survives the
 * finger sliding off the pad, and the centre is quiet. Coordinates are
 * normalized offsets from the pad centre with +y downward.
 */
@RunWith(AndroidJUnit4::class)
class DpadResolverTest {

    private fun resolve(x: Float, y: Float) = DpadResolver.resolve(x, y)

    @Test
    fun centreIsNeutral() {
        assertEquals(emptySet<DpadDirection>(), resolve(0f, 0f))
    }

    @Test
    fun insideTheCentreSquareIsNeutral() {
        assertEquals(emptySet<DpadDirection>(), resolve(0.3f, 0.3f))
    }

    @Test
    fun justPastTheThresholdRegisters() {
        assertEquals(setOf(DpadDirection.Right), resolve(0.34f, 0f))
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

    /**
     * Holding Down and sliding right: Right must engage on crossing the vertical
     * threshold, not after the finger has rotated far enough to leave Down's
     * angular zone. An angle-based resolver held at the pad's rim needs nx ≈ 0.47
     * here and so feels slow to catch the turn.
     */
    @Test
    fun slidingRightWhileHoldingDownEngagesRightOnCrossingTheThreshold() {
        assertEquals(setOf(DpadDirection.Down), resolve(0.3f, 1f))
        assertEquals(setOf(DpadDirection.Down, DpadDirection.Right), resolve(0.34f, 1f))
    }

    /** The turn costs the same travel wherever on the arm the thumb sits. */
    @Test
    fun theEngagePointDoesNotDriftWithDistanceFromCentre() {
        assertEquals(setOf(DpadDirection.Down, DpadDirection.Right), resolve(0.34f, 0.5f))
        assertEquals(setOf(DpadDirection.Down, DpadDirection.Right), resolve(0.34f, 2f))
    }

    /** Sliding off the pad mid-jump must not drop the direction. */
    @Test
    fun aPositionBeyondThePadKeepsItsDirection() {
        assertEquals(setOf(DpadDirection.Right), resolve(4f, 0f))
        assertEquals(setOf(DpadDirection.Up, DpadDirection.Right), resolve(3f, -3f))
    }

    @Test
    fun aShakyThumbAimingUpStaysCardinal() {
        assertEquals(setOf(DpadDirection.Up), resolve(0.2f, -1f))
    }

    @Test
    fun aRaisedThresholdDemandsMoreTravel() {
        assertEquals(emptySet<DpadDirection>(), DpadResolver.resolve(0.4f, 0f, threshold = 0.5f))
        assertEquals(setOf(DpadDirection.Right), DpadResolver.resolve(0.6f, 0f, threshold = 0.5f))
    }

    @Test
    fun aDiagonalRatioDropsTheWeakerAxis() {
        // Down dominates: with a 0.8 ratio Right has to get much closer to it.
        assertEquals(
            setOf(DpadDirection.Down),
            DpadResolver.resolve(0.4f, 1f, diagonalRatio = 0.8f),
        )
        assertEquals(
            setOf(DpadDirection.Down, DpadDirection.Right),
            DpadResolver.resolve(0.9f, 1f, diagonalRatio = 0.8f),
        )
    }

    @Test
    fun aDiagonalRatioOfZeroLeavesBothAxesAlone() {
        assertEquals(
            setOf(DpadDirection.Down, DpadDirection.Right),
            DpadResolver.resolve(0.4f, 1f, diagonalRatio = 0f),
        )
    }

    /** A ratio only ever suppresses; it cannot invent a direction. */
    @Test
    fun aDiagonalRatioNeverEngagesAnAxisUnderTheThreshold() {
        assertEquals(
            setOf(DpadDirection.Down),
            DpadResolver.resolve(0.2f, 1f, diagonalRatio = 0.9f),
        )
    }
}
