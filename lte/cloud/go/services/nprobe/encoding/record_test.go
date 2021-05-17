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
	"encoding/hex"
	"reflect"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

type timeTest struct {
	in  string
	ok  bool
	out string
}

var (
	generalizedTimeTestData = []timeTest{
		{"2021-02-18T05:13:26.019519+00:00", true, "32303231303231383035313332362e303139"},
	}

	// This is a minimal bearer activation record that should be parsed correctly.
	encodedRecord = []byte{
		0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x5f, 0x00, 0x00, 0x00, 0xda, 0x00, 0x0e, 0x00, 0x01,
		0x52, 0xe8, 0xbb, 0xa0, 0x6a, 0x18, 0x49, 0x16, 0xb0, 0x78, 0x3e, 0x23, 0x8b, 0x49, 0x68, 0x0c,
		0x08, 0x66, 0xcb, 0x39, 0x79, 0x08, 0x44, 0xa8, 0x00, 0x06, 0x00, 0x08, 0x73, 0x65, 0x73, 0x73,
		0x69, 0x6f, 0x6e, 0x64, 0x00, 0x11, 0x00, 0x13, 0x49, 0x4d, 0x53, 0x49, 0x30, 0x30, 0x31, 0x30,
		0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x00, 0x08, 0x00, 0x04, 0x00,
		0x00, 0x00, 0x22, 0x00, 0x09, 0x00, 0x08, 0x16, 0x7f, 0x47, 0x4d, 0x46, 0x46, 0xc9, 0xa7, 0xa2,
		0x81, 0xd7, 0x80, 0x08, 0x04, 0x00, 0x02, 0x02, 0x04, 0x08, 0x0f, 0x04, 0x81, 0x00, 0xa3, 0x19,
		0xa0, 0x17, 0x80, 0x12, 0x32, 0x30, 0x32, 0x31, 0x30, 0x35, 0x31, 0x35, 0x31, 0x35, 0x33, 0x33,
		0x30, 0x38, 0x2e, 0x30, 0x39, 0x33, 0x81, 0x01, 0x00, 0x84, 0x01, 0x00, 0xa9, 0x30, 0x30, 0x2e,
		0x80, 0x01, 0x03, 0xa1, 0x29, 0x81, 0x10, 0x04, 0x08, 0x06, 0x04, 0x05, 0x00, 0x08, 0x03, 0x01,
		0x03, 0x01, 0x01, 0x02, 0x03, 0x01, 0x07, 0x83, 0x13, 0x49, 0x4d, 0x53, 0x49, 0x30, 0x30, 0x31,
		0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x86, 0x00, 0x92, 0x08,
		0x08, 0x66, 0xcb, 0x39, 0x79, 0x08, 0x44, 0xa8, 0x94, 0x01, 0x15, 0xba, 0x1f, 0x80, 0x04, 0x00,
		0x00, 0xbf, 0x6a, 0xa1, 0x17, 0xa5, 0x15, 0x81, 0x01, 0x00, 0xa2, 0x10, 0x81, 0x0e, 0x31, 0x39,
		0x32, 0x2e, 0x31, 0x36, 0x38, 0x2e, 0x36, 0x30, 0x2e, 0x31, 0x34, 0x32, 0xbf, 0x24, 0x4a, 0x85,
		0x1a, 0x49, 0x4d, 0x53, 0x49, 0x30, 0x30, 0x31, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
		0x30, 0x30, 0x30, 0x31, 0x2d, 0x38, 0x39, 0x38, 0x35, 0x38, 0x37, 0x95, 0x01, 0x01, 0xb7, 0x29,
		0x81, 0x27, 0x20, 0x38, 0x32, 0x20, 0x30, 0x30, 0x20, 0x66, 0x31, 0x20, 0x31, 0x30, 0x20, 0x30,
		0x30, 0x20, 0x30, 0x31, 0x20, 0x30, 0x30, 0x20, 0x66, 0x31, 0x20, 0x31, 0x30, 0x20, 0x30, 0x30,
		0x20, 0x30, 0x30, 0x20, 0x30, 0x61, 0x20, 0x30, 0x61,
	}
)

func TestGeneralizedTime(t *testing.T) {
	for i, test := range generalizedTimeTestData {
		ptime, _ := time.Parse(time.RFC3339Nano, test.in)
		ret := encodeGeneralizedTime(ptime)
		bOut, _ := hex.DecodeString(test.out)
		if !reflect.DeepEqual(bOut, ret) {
			t.Errorf("#%d: Bad result: %q → %v (expected %v)\n %v", i, test.in, ret, bOut, hex.Dump(ret))
		}
	}
}

func TestEpsIRIRecord(t *testing.T) {
	var record EpsIRIRecord
	if err := record.Decode(encodedRecord); err != nil {
		t.Errorf("Decoding record failed: %v", err)
	}

	assert.Equal(t, HeaderVersion, record.Header.Version)
	assert.Equal(t, uint64(0x866cb39790844a8), record.Header.CorrelationID)
	assert.Equal(t, "52e8bba0-6a18-4916-b078-3e238b49680c", record.Header.XID.String())

	assert.Equal(t, BearerDeactivation, record.Payload.EPSEvent)
	assert.Equal(t, InitiatorNotAvailable, record.Payload.Initiator)
	assert.Equal(t, GetOID(), record.Payload.Hi2epsDomainID)
}
