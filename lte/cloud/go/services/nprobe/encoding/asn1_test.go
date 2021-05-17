/*
Copyright 2020 The Magma Authors.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree.

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package encoding

import (
	"encoding/asn1"
	"testing"

	"github.com/stretchr/testify/assert"
)

var (
	// This is a minimal bearer activation record that should be parsed correctly.
	encodedPayload = []byte{
		0xa1, 0x81, 0xed, 0x80, 0x08, 0x04, 0x00, 0x02, 0x02, 0x04, 0x08, 0x0f, 0x04, 0x81, 0x00, 0xa3,
		0x19, 0xa0, 0x17, 0x80, 0x12, 0x32, 0x30, 0x32, 0x31, 0x30, 0x35, 0x31, 0x35, 0x31, 0x35, 0x32,
		0x39, 0x32, 0x31, 0x2e, 0x31, 0x32, 0x36, 0x81, 0x01, 0x00, 0x84, 0x01, 0x00, 0xa9, 0x30, 0x30,
		0x2e, 0x80, 0x01, 0x03, 0xa1, 0x29, 0x81, 0x10, 0x04, 0x08, 0x06, 0x04, 0x05, 0x00, 0x08, 0x03,
		0x01, 0x03, 0x01, 0x01, 0x02, 0x03, 0x01, 0x07, 0x83, 0x13, 0x49, 0x4d, 0x53, 0x49, 0x30, 0x30,
		0x31, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x86, 0x00, 0x92,
		0x08, 0x08, 0x66, 0xcb, 0x39, 0x79, 0x08, 0x44, 0xa8, 0x94, 0x01, 0x12, 0xba, 0x1f, 0x80, 0x04,
		0x00, 0x00, 0xbf, 0x6a, 0xa1, 0x17, 0xa5, 0x15, 0x81, 0x01, 0x00, 0xa2, 0x10, 0x81, 0x0e, 0x31,
		0x39, 0x32, 0x2e, 0x31, 0x36, 0x38, 0x2e, 0x36, 0x30, 0x2e, 0x31, 0x34, 0x32, 0xbf, 0x24, 0x60,
		0x81, 0x05, 0x00, 0xc0, 0xa8, 0x80, 0x0c, 0x82, 0x0a, 0x6d, 0x61, 0x67, 0x6d, 0x61, 0x2e, 0x69,
		0x70, 0x76, 0x34, 0x85, 0x1a, 0x49, 0x4d, 0x53, 0x49, 0x30, 0x30, 0x31, 0x30, 0x31, 0x30, 0x30,
		0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x2d, 0x35, 0x38, 0x33, 0x39, 0x31, 0x31, 0x87,
		0x01, 0x06, 0x8a, 0x01, 0x01, 0xb7, 0x29, 0x81, 0x27, 0x20, 0x38, 0x32, 0x20, 0x30, 0x30, 0x20,
		0x66, 0x31, 0x20, 0x31, 0x30, 0x20, 0x30, 0x30, 0x20, 0x30, 0x31, 0x20, 0x30, 0x30, 0x20, 0x66,
		0x31, 0x20, 0x31, 0x30, 0x20, 0x30, 0x30, 0x20, 0x30, 0x30, 0x20, 0x30, 0x61, 0x20, 0x30, 0x61,
	}
)

func TestEpsIRIContent(t *testing.T) {
	var content EpsIRIContent
	if _, err := asn1.UnmarshalWithParams(encodedPayload, &content, IRIBeginRecord); err != nil {
		t.Errorf("Unmarshal failed: %v", err)
	}

	assert.Equal(t, content.EPSEvent, BearerActivation)
	assert.Equal(t, content.Initiator, InitiatorNotAvailable)
	assert.Equal(t, content.Hi2epsDomainID, GetOID())

	imsi := []byte("IMSI001010000000001")
	assert.Equal(t, imsi, content.PartyInformation[0].PartyIdentity.IMSI)

	apn := []byte("magma.ipv4")
	assert.Equal(t, apn, content.EPSSpecificParameters.APN)

	bid := []byte("IMSI001010000000001-583911")
	assert.Equal(t, bid, content.EPSSpecificParameters.EPSBearerIdentity)
	assert.Equal(t, content.EPSSpecificParameters.BearerActivationType, DefaultBearer)
	assert.Equal(t, content.EPSSpecificParameters.RATType, []byte{RatTypeEutran})

	marshaledContent, err := asn1.MarshalWithParams(content, IRIBeginRecord)
	if err != nil {
		t.Errorf("Marshal failed: %v", err)
	}
	assert.Equal(t, marshaledContent, encodedPayload)
}
