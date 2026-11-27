package com.example.navbridge

object LapAnalysisStore {
    data class Payload(
        val session: RaceSession,
        val laps: List<LapDetail>,
        val traces: Map<Int, TraceData>,
        val track: RaceTrack? = null,
        val satellite: TrackSatellite? = null,
        val base: String = "",
        val localMode: Boolean = false,
        val offlineMode: Boolean = false,
    )

    var payload: Payload? = null
}
