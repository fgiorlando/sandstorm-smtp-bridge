import smtplib
import os
from email.MIMEMultipart import MIMEMultipart
from email.MIMEBase import MIMEBase
from email.MIMEText import MIMEText
from email.Utils import COMMASPACE, formatdate
from email import Encoders
import sys

PORT = 30125

def send_mail(send_to, send_from='test2@test.com', subject='test email', text='this is a test', files=[__file__], port=PORT):
    assert isinstance(send_to, list)
    assert isinstance(files, list)
    server = send_to[0].split('@')[-1]

    msg = MIMEMultipart()
    msg['From'] = send_from
    msg['To'] = COMMASPACE.join(send_to)
    msg['Date'] = formatdate(localtime=True)
    msg['Subject'] = subject

    msg.attach(MIMEText(text))

    for f in files:
        part = MIMEBase('application', "octet-stream")
        part.set_payload(open(f, "rb").read())
        Encoders.encode_base64(part)
        part.add_header('Content-Disposition', 'attachment; filename="%s"' % os.path.basename(f))
        msg.attach(part)

    print server, port
    smtp = smtplib.SMTP(server, port)
    smtp.set_debuglevel(True)
    smtp.helo()
    smtp.sendmail(send_from, send_to, msg.as_string())
    smtp.quit()
    smtp.close()

if __name__ == '__main__':
    send_mail(sys.argv[1].split(','))
