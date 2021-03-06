/*! \file    sdp.h
 * \author   Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU Affero General Public License v3
 * \brief    SDP processing (headers)
 * \details  Implementation (based on the Sofia-SDP stack) of the SDP
 * parser/merger/generator in the gateway. Each SDP coming from peers is
 * stripped/anonymized before it is passed to the plugins: all
 * DTLS/ICE/transport related information is removed, only leaving the
 * relevant information in place. SDP coming from plugins is stripped/anonymized
 * as well, and merged with the proper DTLS/ICE/transport information before
 * it is sent to the peers.
 * 
 * \todo Right now, we only support sessions with up to a single audio
 * and/or a single video stream (as in, a single audio and/or video
 * m-line). Later versions of the gateway will add support for more
 * audio and video streams in the same session. Besides, DataChannels
 * are not supported as of yet either: this is something we'll start
 * working in the future as well.
 * 
 * \ingroup protocols
 * \ref protocols
 */
 
#ifndef _JANUS_SDP_H
#define _JANUS_SDP_H


#include <inttypes.h>
#include <sofia-sip/sdp.h>


/** @name Janus SDP setup
 */
///@{
/*! \brief Janus SDP processor initialization
 * @returns 0 in case of success, -1 in case of an error */
int janus_sdp_init(void);
/*! \brief Janus SDP processor deinitialization */
void janus_sdp_deinit(void);
///@}


/* Parser stuff */
/*! \brief Janus SDP instance */
typedef struct janus_sdp {
	/*! \brief Sofia-SDP parser instance */
	void *parser;
	/*! \brief Sofia-SDP session description */
	void *sdp;
} janus_sdp;

/*! \brief Method to free a Janus SDP instance
 * @param[in] sdp The Janus SDP instance to free */
void janus_sdp_free(janus_sdp *sdp);


/** @name Janus SDP helper methods
 */
///@{
/*! \brief Method to pre-parse a session description
 * \details This method is only used to quickly check how many audio and video lines are in an SDP, and to generate a Janus SDP instance
 * @param[in] jsep_sdp The SDP that the browser peer originated
 * @param[out] audio The number of audio m-lines
 * @param[out] video The number of video m-lines
 * @returns The Janus SDP instance in case of success, NULL in case the SDP is invalid */
janus_sdp *janus_sdp_preparse(const char *jsep_sdp, int *audio, int *video);

/*! \brief Method to parse a session description
 * \details This method will parse a session description coming from a peer, and set up the ICE candidates accordingly
 * @param[in] session The ICE session this session description will modify
 * @param[in] sdp The Janus SDP instance to parse
 * @returns 0 in case of success, -1 in case of an error */
int janus_sdp_parse(janus_ice_handle *session, janus_sdp *sdp);

/*! \brief Method to strip/anonymize a session description
 * @param[in] sdp The session description to strip/anonymize
 * @returns A string containing the stripped/anonymized session description in case of success, NULL if the SDP is invalid */
char *janus_sdp_anonymize(const char *sdp);

/*! \brief Method to merge a stripped session description and the right transport information
 * @param[in] session The ICE session this session description is related to
 * @param[in] sdp The stripped session description to merge
 * @returns A string containing the full session description in case of success, NULL if the SDP is invalid */
char *janus_sdp_merge(janus_ice_handle *session, const char *sdp);
///@}

#endif
