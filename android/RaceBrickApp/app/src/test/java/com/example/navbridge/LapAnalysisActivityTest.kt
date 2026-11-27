package com.example.navbridge

import android.graphics.PointF
import android.widget.CheckBox
import android.widget.LinearLayout
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [34])
class LapAnalysisActivityTest {
    @Test
    fun defaultsToFastestLapOnly() {
        val session = RaceSession(
            slot = 1, id = 1, trackSlot = 2, trackName = "Test",
            startEpoch = 0, durationMs = 60_000, samples = 8, laps = 2,
            lapTimes = listOf(30.0, 25.0),
        )
        val laps = listOf(
            LapDetail(1, 30.0, 4, 500.0, 60.0, 70.0, false),
            LapDetail(2, 25.0, 4, 500.0, 72.0, 80.0, false),
        )
        fun trace(): TraceData = TraceData(
            points = (0 until 4).map { PointF(it.toFloat(), 0f) },
            speeds = listOf(10.0, 50.0, 40.0, 20.0),
            times = listOf(0L, 1000L, 2000L, 3000L),
        )
        LapAnalysisStore.payload = LapAnalysisStore.Payload(
            session, laps, mapOf(1 to trace(), 2 to trace()),
        )
        val controller = Robolectric.buildActivity(LapAnalysisActivity::class.java).setup()
        val row = controller.get().findViewById<LinearLayout>(R.id.llAnalysisLaps)
        val boxes = (0 until row.childCount).map { row.getChildAt(it) as CheckBox }
        assertEquals(listOf("L1", "L2"), boxes.map { it.text.toString() })
        assertEquals(false, boxes[0].isChecked)
        assertEquals(true, boxes[1].isChecked)
        controller.pause().stop().destroy()
    }

    @Test
    fun moreThanTwoLapsShowsTableInsteadOfChart() {
        val session = RaceSession(
            slot = 3, id = 3, trackSlot = 2, trackName = "Test",
            startEpoch = 0, durationMs = 120_000, samples = 16, laps = 4,
            lapTimes = listOf(30.0, 25.0, 27.0, 26.0),
        )
        val laps = listOf(
            LapDetail(1, 30.0, 4, 500.0, 60.0, 70.0, false),
            LapDetail(2, 25.0, 4, 500.0, 72.0, 80.0, false),
            LapDetail(3, 27.0, 4, 500.0, 65.0, 75.0, false),
            LapDetail(4, 26.0, 4, 500.0, 68.0, 78.0, false),
        )
        fun trace(): TraceData = TraceData(
            points = (0 until 4).map { PointF(it.toFloat(), 0f) },
            speeds = listOf(10.0, 50.0, 40.0, 20.0),
            times = listOf(0L, 1000L, 2000L, 3000L),
        )
        LapAnalysisStore.payload = LapAnalysisStore.Payload(
            session, laps, (1..4).associateWith { trace() },
        )
        val controller = Robolectric.buildActivity(LapAnalysisActivity::class.java).setup()
        val activity = controller.get()
        val row = activity.findViewById<LinearLayout>(R.id.llAnalysisLaps)
        val boxes = (0 until row.childCount).map { row.getChildAt(it) as CheckBox }
        boxes[2].isChecked = true
        boxes[3].isChecked = true
        assertEquals(3, boxes.count { it.isChecked })
        assertEquals(android.view.View.GONE, activity.findViewById<android.view.View>(R.id.speedChart).visibility)
        assertEquals(android.view.View.VISIBLE, activity.findViewById<android.view.View>(R.id.llAnalysisTable).visibility)
        controller.pause().stop().destroy()
    }

    @Test
    @Config(sdk = [34], qualifiers = "land")
    fun landscapeLayoutLoadsFastestLap() {
        val session = RaceSession(
            slot = 2, id = 2, trackSlot = 2, trackName = "Test",
            startEpoch = 0, durationMs = 60_000, samples = 4, laps = 2,
            lapTimes = listOf(30.0, 25.0),
        )
        val laps = listOf(
            LapDetail(1, 30.0, 4, 500.0, 60.0, 70.0, false),
            LapDetail(2, 25.0, 4, 500.0, 72.0, 80.0, false),
        )
        fun trace(): TraceData = TraceData(
            points = (0 until 4).map { PointF(it.toFloat(), 0f) },
            speeds = listOf(10.0, 50.0, 40.0, 20.0),
            times = listOf(0L, 1000L, 2000L, 3000L),
        )
        LapAnalysisStore.payload = LapAnalysisStore.Payload(
            session, laps, mapOf(1 to trace(), 2 to trace()),
        )
        val controller = Robolectric.buildActivity(LapAnalysisActivity::class.java).setup()
        val row = controller.get().findViewById<LinearLayout>(R.id.llAnalysisLaps)
        val boxes = (0 until row.childCount).map { row.getChildAt(it) as CheckBox }
        assertEquals(false, boxes[0].isChecked)
        assertEquals(true, boxes[1].isChecked)
        controller.pause().stop().destroy()
    }
}
