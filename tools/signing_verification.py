import argparse, os, base64, json
import base64
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.x509 import load_pem_x509_certificate
from cryptography.hazmat.primitives.asymmetric import utils
from cryptography.hazmat.primitives.asymmetric import rsa
from hashlib import sha256

class bcolors:
	HEADER = '\033[95m'
	OKBLUE = '\033[94m'
	OKCYAN = '\033[96m'
	OKGREEN = '\033[92m'
	WARNING = '\033[93m'
	FAIL = '\033[91m'
	ENDC = '\033[0m'
	BOLD = '\033[1m'
	UNDERLINE = '\033[4m'

def print_colored(string, color):
	print(color + string + bcolors.ENDC)

def print_colored_ok(string):
	print_colored(string, bcolors.OKGREEN)

def print_colored_error(string):
	print_colored(string, bcolors.FAIL)

def print_colored_title_msg(title, msg):
	print(bcolors.HEADER + title + bcolors.ENDC + ": " + msg)

def pubkey_from_cert(cert_path):
	if not os.path.exists(cert_path):
		raise FileNotFoundError("Certificate file does not exist")

	with open(cert_path, 'r') as cert_file:
			cert_str = cert_file.read()
			if not cert_str.startswith('-----BEGIN CERTIFICATE-----'):
					cert_str = '-----BEGIN CERTIFICATE-----\n' + cert_str
			if not cert_str.endswith('-----END CERTIFICATE-----\n'):
					cert_str = cert_str + '\n-----END CERTIFICATE-----\n'
			cert_obj = load_pem_x509_certificate(bytes(cert_str, 'utf-8'))
			return cert_obj.public_key()

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("-p", "--packet", help="Input packet in b64 string", required=True)
	parser.add_argument("-s", "--signature", help="Packet signature in b64 string", required=True)
	parser.add_argument("-c", "--cert", help="Device certificate file path", required=True)
	args = parser.parse_args()

	# Packet decoding
	try:
		packet_json_bytes = base64.b64decode(args.packet)
	except Exception as e:
		print_colored_error("Packet b64 decoding error: " + str(e))
		exit()

	try:
		packet_json_obj_form = json.dumps(json.loads(packet_json_bytes.decode()), indent=2)
		print_colored_title_msg("Decoded packet", "\n" + packet_json_obj_form)
	except Exception as e:
		print_colored_error("Packet JSON decoding error: " + str(e))
		exit()

	# Signature decoding
	try:
		sig_bytes = base64.b64decode(args.signature)
		# print_colored_title_msg("Signature r hex", sig_bytes[0:32].hex())
		# print_colored_title_msg("Signature s hex", sig_bytes[32:64].hex())
	except Exception as e:
		print_colored_error("Signature b64 decoding error: " + str(e))
		exit()

	# Pubkey extraction from pem certificate file
	try:
		pubkey = pubkey_from_cert(os.path.normpath(args.cert))
		# print_colored_title_msg("PubKey", "\n" + pub_key_pem.decode())
	except Exception as e:
		print_colored_error("Pubkey extraction error: " + str(e))
		exit()

	hash = sha256(str(args.packet).encode('ascii'))
	print(hash.hexdigest())

	# Verificatin test
	try:
		pubkey.verify(
        sig_bytes,
        hash.digest(),
        padding.PKCS1v15(),
        utils.Prehashed(hashes.SHA256()))
		print_colored_ok("---Verification Success---")
	except Exception as e:
		print_colored_error("Verification error: " + str(type(e)))
		exit()

	exit()


if __name__=="__main__":
	main()